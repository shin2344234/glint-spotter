#include "hook/tick.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/rtti.h"
#include "game/signatures.h"
#include "hook/vtable.h"
#include "game/mapicon.h"
#include "game/player.h"
#include "game/aim.h"
#include "game/snapshot.h"
#include "game/nearest.h"
#include "game/actors.h"
#include "game/camera.h"
#include "game/dump.h"
#include "game/physics.h"
#include "core/settings.h"

// Shared with thunk.asm. C linkage so the names match what MASM emits.
extern "C" void* gs_minimapOriginal = nullptr;
extern "C" void gs_MinimapTickThunk();

namespace
{
    gs::vtable::Swap g_swap;
    std::atomic<uint64_t> g_count{0};
    std::atomic<uint32_t> g_thread{0};
    std::atomic<bool> g_probe{false};

    // The diff probe's state. Sampled every kSampleTicks ticks, compared to the
    // sample before, and only the float fields that moved are logged. Position
    // shows up as fields that drift by walking speed; heading as one that
    // wraps around a circle. Everything else is noise to be filtered offline.
    constexpr size_t kBytes = gs::sig::kRootControlSize;
    constexpr uint64_t kSampleTicks = 180;   // about 3 s at 60 Hz
    constexpr int kMaxLines = 32;
    uint8_t g_prev[kBytes];
    bool g_havePrev = false;
    uint64_t g_samples = 0;

    // Further targets, diffed the same way. Guarded by the tick's own thread
    // for reads; adds and drops come from the worker and are atomic swaps.
    constexpr size_t kExtraMax = 0x400;
    constexpr int kExtraSlots = 12;
    struct Extra
    {
        char label[32];
        std::atomic<void*> object{nullptr};
        size_t bytes = 0;
        uint8_t prev[kExtraMax];
        bool havePrev = false;
    };
    Extra g_extras[kExtraSlots];

    // A mark asked for from another thread, placed here on the game's.
    std::atomic<bool> g_markPending{false};
    float g_markX = 0, g_markZ = 0;
    char g_markLabel[48] = "GlintSpotter";
    std::atomic<void*> g_worldRoot{nullptr};

    // Copy the object out inside a handler; the compare runs on our copy.
    bool Snapshot(const void* self, uint8_t* out, size_t bytes = kBytes)
    {
        __try
        {
            if (!gs::rtti::Readable(self, bytes)) return false;
            memcpy(out, self, bytes);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool PlausibleFloat(float f)
    {
        if (!std::isfinite(f)) return false;
        const float a = std::fabs(f);
        return a == 0.0f || (a > 1e-4f && a < 1e6f);
    }

    // Log every float field that moved between two samples, plus every byte
    // that flipped, because a flag is a byte and a heading is a float.
    void Diff(const char* tag, const uint8_t* prev, const uint8_t* cur, size_t bytes)
    {
        int lines = 0;
        for (size_t off = 8; off + 4 <= bytes && lines < kMaxLines; off += 4)
        {
            float a, b;
            memcpy(&a, prev + off, 4);
            memcpy(&b, cur + off, 4);
            if (a == b) continue;
            if (PlausibleFloat(a) && PlausibleFloat(b))
            {
                const float d = std::fabs(b - a);
                if (d >= 1e-3f && d <= 5000.0f)
                {
                    GS_LOG("[%s %llu] +0x%03zX  %12.4f -> %12.4f  (d %.4f)", tag,
                           static_cast<unsigned long long>(g_samples), off, a, b, d);
                    ++lines;
                    continue;
                }
            }
            uint32_t ua, ub;
            memcpy(&ua, prev + off, 4);
            memcpy(&ub, cur + off, 4);
            // Small integers flipping are flags and enums. A value going from
            // zero to anything, or back, is a key or a pointer being set or
            // cleared, which is what a mode turning on or a target being
            // acquired looks like. Session eleven filtered both as churn.
            if ((ua < 0x10000 && ub < 0x10000) || ua == 0 || ub == 0)
            {
                GS_LOG("[%s %llu] +0x%03zX  0x%08X -> 0x%08X", tag,
                       static_cast<unsigned long long>(g_samples), off, ua, ub);
                ++lines;
            }
            // An 8-byte field that now holds a pointer to something with RTTI
            // is worth naming: an aim target would be exactly that.
            if ((off & 7) == 0 && off + 8 <= bytes)
            {
                uint64_t qa, qb;
                memcpy(&qa, prev + off, 8);
                memcpy(&qb, cur + off, 8);
                if (qa != qb && qb > 0x10000 && (qb & 7) == 0 &&
                    gs::rtti::Readable(reinterpret_cast<const void*>(qb), 8))
                {
                    const uint64_t vt = *reinterpret_cast<const uint64_t*>(qb);
                    const char* n = gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt));
                    if (n && lines < kMaxLines)
                    {
                        GS_LOG("[%s %llu] +0x%03zX  -> 0x%016llX is %s", tag,
                               static_cast<unsigned long long>(g_samples), off,
                               static_cast<unsigned long long>(qb), n);
                        ++lines;
                    }
                }
            }
        }
        if (lines == kMaxLines) GS_LOG("[%s %llu]   ... more changed, capped", tag,
                                       static_cast<unsigned long long>(g_samples));
    }

    void Probe(const void* self)
    {
        uint8_t cur[kBytes];
        if (!Snapshot(self, cur)) return;
        ++g_samples;
        if (g_havePrev) Diff("root", g_prev, cur, kBytes);
        memcpy(g_prev, cur, kBytes);
        g_havePrev = true;

        // The live position, the origin of any aim ray, read here where it
        // is freshest.
        const gs::player::Pos pp = gs::player::Read();
        if (pp.valid) GS_LOG("[player %llu] (%.3f, %.3f, %.3f)",
                             static_cast<unsigned long long>(g_samples), pp.x, pp.y, pp.z);
        gs::camera::LogSample(g_samples);
        {
            static uint64_t lastCalls = 0, lastTicks = 0;
            const uint64_t calls = gs::camera::Calls(), ticks = g_count.load();
            if (lastTicks && ticks > lastTicks)
                GS_LOG("[cam %llu] update ran %.2f times per tick, this 0x%p", static_cast<unsigned long long>(g_samples),
                       static_cast<double>(calls - lastCalls) / static_cast<double>(ticks - lastTicks),
                       reinterpret_cast<void*>(gs::camera::This()));
            lastCalls = calls; lastTicks = ticks;
        }

        for (Extra& e : g_extras)
        {
            void* obj = e.object.load();
            if (!obj || !e.bytes) continue;
            uint8_t ecur[kExtraMax];
            if (!Snapshot(obj, ecur, e.bytes)) continue;
            if (e.havePrev) Diff(e.label, e.prev, ecur, e.bytes);
            memcpy(e.prev, ecur, e.bytes);
            e.havePrev = true;
        }
    }
}

namespace
{
    uint64_t g_lastRefreshTick = 0;

    // The world map root control does not exist until the map is opened
    // once. Session twenty-two placed a pin on what the sweep offered
    // instead, a registry entry, and the game went down. Pins asked for
    // before the spy has seen the real root wait here.
    struct Pending { float x, y, z; char label[16]; };
    constexpr int kPendingMax = 32;
    Pending g_pending[kPendingMax];
    int g_pendingN = 0;

    bool PendingNear(float x, float z, float radius)
    {
        for (int i = 0; i < g_pendingN; ++i)
        {
            const float dx = g_pending[i].x - x, dz = g_pending[i].z - z;
            if (dx * dx + dz * dz <= radius * radius) return true;
        }
        return false;
    }

    void FlushPending()
    {
        if (g_pendingN == 0) return;
        void* root = gs::mapicon::LastWorldRoot();
        if (!root) return;
        GS_LOG("[mark] the world map root exists now; placing %d queued pin(s)", g_pendingN);
        for (int i = 0; i < g_pendingN; ++i)
            gs::mapicon::PlacePinNow(root, g_pending[i].x, g_pending[i].y, g_pending[i].z, g_pending[i].label);
        g_pendingN = 0;
    }

    void PlaceAt(float tx, float ty, float tz, const char* how, const char* label, const gs::player::Pos& pp, float dedupe)
    {
        if (gs::mapicon::PinNear(tx, tz, dedupe) || PendingNear(tx, tz, dedupe))
        {
            GS_LOG("[mark] a pin already sits within %.0f units of (%.1f, %.1f); not placing another", dedupe, tx, tz);
            return;
        }
        const float dx = tx - pp.x, dz = tz - pp.z;
        GS_LOG("[mark] target %.1f units away via %s; placing a %s pin at (%.1f, %.1f, %.1f)",
               std::sqrt(dx * dx + dz * dz), how, label, tx, ty, tz);
        // Only a root the spy has seen the game call slot 170 on. The
        // sweep's candidate is never used for a call.
        void* root = gs::mapicon::LastWorldRoot();
        if (!root)
        {
            if (g_pendingN < kPendingMax)
            {
                Pending& p = g_pending[g_pendingN++];
                p.x = tx; p.y = ty; p.z = tz;
                strncpy_s(p.label, sizeof(p.label), label, _TRUNCATE);
                GS_LOG("[mark] the world map has not been opened this session, so its root does not exist yet; "
                       "pin queued (%d waiting). Open the map once and it appears.", g_pendingN);
            }
            else GS_LOG_ERR("[mark] %d pins already waiting for the map to be opened; this one is dropped", g_pendingN);
            return;
        }
        gs::mapicon::PlacePinNow(root, tx, ty, tz, label);
    }

    // Three pins at known offsets from the player, once a session, the first
    // time the flash is held.
    //
    // Every pin the mod has ever placed went to something twenty-odd metres
    // away, and Seth has reported every one of them as sitting next to his
    // character. Two different faults predict exactly that: the mod picking a
    // near object when it should pick a far one, or the map ignoring the
    // position and drawing every pin on the player. Twenty metres is a few
    // pixels of world map, so the reports cannot tell them apart.
    //
    // These can. If "N200" appears two hundred metres north and "E100" a
    // hundred metres east, the coordinate path is sound and the fault is the
    // pick. If all three land on the player, the fault is the call.
    bool g_calibrated = false;

    void Calibrate(const gs::player::Pos& pp)
    {
        if (g_calibrated) return;
        g_calibrated = true;
        GS_LOG("[cal] placing three pins at known offsets from (%.1f, %.1f, %.1f): "
               "\"Me\" on you, \"N200\" 200 metres north, \"E100\" 100 metres east. "
               "Where they land says whether a pin goes where it is asked to.",
               pp.x, pp.y, pp.z);
        PlaceAt(pp.x, pp.y, pp.z, "calibration, your own position", "Me", pp, 0.0f);
        PlaceAt(pp.x, pp.y, pp.z + 200.0f, "calibration, 200 north", "N200", pp, 0.0f);
        PlaceAt(pp.x + 100.0f, pp.y, pp.z, "calibration, 100 east", "E100", pp, 0.0f);
    }

    // The view ray. The camera's own forward when its object is in hand,
    // otherwise the body's facing held level. The origin is eye height.
    struct View
    {
        float ox = 0, oy = 0, oz = 0;
        float fx = 0, fy = 0, fz = 1;
        bool camera = false;
    };

    int g_sayOriginLeft = 6;

    bool ViewRay(const gs::player::Pos& pp, View* v)
    {
        v->ox = pp.x; v->oy = pp.y + 1.6f; v->oz = pp.z;
        const gs::camera::Pose cam = gs::camera::Read();
        if (cam.fwdValid)
        {
            v->fx = cam.fwd[0]; v->fy = cam.fwd[1]; v->fz = cam.fwd[2];
            v->camera = true;
            // The crosshair ray leaves the camera, which sits `dist` behind
            // the pivot and a little above it, not the player's eye. At a
            // grazing pitch that height is tens of units on the ground.
            // The pivot is in the sub-level's frame; the player's world
            // minus local offset moves it to the map's.
            if (cam.dist > 0.5f && cam.dist < 30.0f)
            {
                const float cx = cam.pivot[0] + (pp.x - pp.lx) - cam.fwd[0] * cam.dist;
                const float cy = cam.pivot[1] + (pp.y - pp.ly) - cam.fwd[1] * cam.dist + 0.6f;
                const float cz = cam.pivot[2] + (pp.z - pp.lz) - cam.fwd[2] * cam.dist;
                // The pivot is in the sub-level's frame and the player's world
                // minus local offset moves it to the map's, which is right only
                // while both are in the same frame. When the answer lands far
                // from the player it is not the camera, so the eye is used.
                const float dx = cx - pp.x, dy = cy - pp.y, dz = cz - pp.z;
                if (dx * dx + dy * dy + dz * dz <= 40.0f * 40.0f)
                {
                    v->ox = cx; v->oy = cy; v->oz = cz;
                }
                else if (g_sayOriginLeft > 0)
                {
                    --g_sayOriginLeft;
                    GS_LOG("[auto] the camera origin came out at (%.1f, %.1f, %.1f), %.0f from the player at (%.1f, %.1f, %.1f); using the eye",
                           cx, cy, cz, std::sqrt(dx * dx + dy * dy + dz * dz), pp.x, pp.y, pp.z);
                }
            }
            return true;
        }
        float fx = 0, fz = 0;
        if (!gs::nearest::ForwardFromQuat(pp.q, &fx, &fz)) return false;
        v->fx = fx; v->fy = 0; v->fz = fz;
        v->camera = false;
        return true;
    }

    // Where the view ray meets the plane of the player's feet. Only with a
    // camera pitch, and only looking down.
    bool GroundPoint(const View& v, float* gx, float* gy, float* gz, float* t)
    {
        if (!v.camera || v.fy > -0.03f) return false;
        const float tt = 1.6f / (-v.fy);
        if (tt > 120.0f) return false;
        *t = tt;
        *gx = v.ox + v.fx * tt; *gy = v.oy + v.fy * tt; *gz = v.oz + v.fz * tt;
        return true;
    }

    int CastView(const View& v, float maxAlong, float radius, float spread, bool glintOnly,
                 gs::nearest::Candidate* c, int n, gs::nearest::Candidate* miss, int missN,
                 float nearAll = 0.0f)
    {
        if (v.camera)
            return gs::nearest::Cast3D(gs::player::Actor(), v.ox, v.oy, v.oz, v.fx, v.fy, v.fz,
                                       maxAlong, radius, spread, glintOnly, c, n, miss, missN, nearAll);
        return gs::nearest::Cast(gs::player::Actor(), v.ox, v.oy, v.oz, v.fx, v.fz,
                                 maxAlong, radius, spread, glintOnly, c, n, miss, missN);
    }

    // The classes in an entity's component block, one line. Session
    // twenty-four's aimed object was a ClientNormalInGameActor with no
    // gimmick component; this says what it carries instead.
    void DescribeComponents(const char* tag, uintptr_t entity)
    {
        char line[900];
        int w = 0;
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(entity + 0x68), 8)) return;
            const uintptr_t comps = *reinterpret_cast<const uintptr_t*>(entity + 0x68);
            if (comps < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(comps), 0x80)) return;
            for (uintptr_t off = 0; off < 0x80 && w < 800; off += 8)
            {
                const uintptr_t c = *reinterpret_cast<const uintptr_t*>(comps + off);
                if (c < 0x10000 || (c & 7) != 0 || !gs::rtti::Readable(reinterpret_cast<const void*>(c), 8)) continue;
                const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(c);
                const char* n = gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt));
                if (!n) continue;
                if (n[0] == '.') n += 4;
                const char* end = strstr(n, "@");
                const int len = end ? static_cast<int>(end - n) : static_cast<int>(strlen(n));
                const int k = _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " +%02llX:%.*s", static_cast<unsigned long long>(off), len, n);
                if (k < 0) break;
                w += k;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        line[w] = 0;
        GS_LOG("[mark]     %s components:%s", tag, line);
    }

    // Flash on: find out which node the flash is lighting. Pin nothing until
    // that is known.
    //
    // Seth's rule, and it is the right one: only the glint gets a pin. The mod
    // cannot yet tell which node is glinting. Two fields named from
    // disassembly both flipped with the flash off, and marking every
    // collectible nearby instead was a guess wearing the feature's clothes,
    // so it is gone.
    //
    // What is left is the measurement that settles it. While the flash is on,
    // the four nearest nodes on the mark list have their gimmick component
    // and their effect component's activity recorded; two seconds after the
    // flash ends, the same four again. The node Seth sees glowing is one of
    // them, and the field that means revealed is the one that moved for that
    // node and stayed put on the other three.
    uint32_t g_flashOnMs = 0;
    bool g_flashWas = false;
    bool g_probedThisPress = false;
    int g_flashProbesLeft = 3;
    int g_setListingsLeft = 2;
    int g_targetLogsLeft = 12;
    uint32_t g_targetLastMs = 0;
    int g_heldWinsLeft = 40;   // how many times the log says the target took the pick
    int g_loadingLogsLeft = 4;
    uint32_t g_heldEid = 0;
    uint32_t g_heldSinceMs = 0;
    float g_heldX = 0, g_heldZ = 0;
    uint32_t g_cooldownUntil = 0;
    int g_autoLogsLeft = 60;
    uint32_t g_autoLastLogMs = 0;
    uintptr_t g_huntEnts[4] = {0, 0, 0, 0};
    uint32_t g_huntEids[4] = {0, 0, 0, 0};
    char g_huntNames[4][48]{};
    int g_huntOn[4] = {0, 0, 0, 0};
    int g_huntN = 0;
    int g_huntLeft = 6;
    uint32_t g_huntLastMs = 0;
    uint32_t g_huntOffMs = 0;

    void AutoMark(uint32_t now)
    {
        const bool flash = gs::aim::FlashActive();
        if (!flash)
        {
            g_flashWas = false;
            if (g_huntN)
            {
                if (!g_huntOffMs) g_huntOffMs = now;
                else if (now - g_huntOffMs > 2000)
                {
                    for (int i = 0; i < g_huntN; ++i)
                    {
                        const int off = gs::dump::EffectActivity(g_huntEnts[i], 0x400);
                        GS_LOG("[hunt] \"%s\" eid %08X: effects %d with the flash on, %d with it off%s",
                               g_huntNames[i], g_huntEids[i], g_huntOn[i], off,
                               (off >= 0 && g_huntOn[i] >= 0 && off != g_huntOn[i]) ? "  <- this one changed" : "");
                        char tag[24];
                        _snprintf_s(tag, sizeof(tag), _TRUNCATE, "off%d", i + 1);
                        gs::dump::GimmickState(tag, g_huntEnts[i], 0x480);
                    }
                    g_huntN = 0;
                    g_huntOffMs = 0;
                }
            }
            return;
        }
        g_huntOffMs = 0;
        if (!g_flashWas)
        {
            g_flashWas = true;
            g_flashOnMs = now;
            g_probedThisPress = false;
        }

        // What the game itself decided the flash lit. The detect component
        // keeps a FindDetectTargetTask at +0x1D0, named by RTTI and never once
        // read in fifty sessions, and the special mode component fills three
        // pointer fields the moment the flash fires. The mod is still guessing
        // which node is glinting from a bearing; one of these objects knows.
        // Half a second after the press, so the task has run, three presses a
        // session, read only.
        if (!g_probedThisPress && g_flashProbesLeft > 0 && now - g_flashOnMs > 500)
        {
            g_probedThisPress = true;
            --g_flashProbesLeft;
            const uintptr_t task = gs::aim::DetectTask();
            if (task)
            {
                GS_LOG("[flash] FindDetectTargetTask at 0x%p", reinterpret_cast<void*>(task));
                // Out to 0x600: session nineteen's probe found the detected
                // actor at +0x540 and session fifty-two's dump stopped at
                // 0x200, which is why the log showed an empty task.
                gs::dump::Pointers("task", task, 0x600);
                gs::dump::Object("taskhex", task, 0x600);
            }
            else
            {
                GS_LOG("[flash] no FindDetectTargetTask on the detect component");
            }
            if (const uintptr_t special = gs::aim::SpecialComponent())
            {
                gs::dump::Pointers("special", special, 0x600);
                gs::dump::Object("specialhex", special, 0x600);
            }
            if (const uintptr_t detect = gs::aim::DetectComponent())
            {
                gs::dump::Pointers("detectp", detect, 0x700);
                gs::dump::Object("detecthex", detect, 0x700);
            }
        }
        const gs::player::Pos pp = gs::player::Read();
        if (!pp.valid) return;

        // Not while the world is loading. Session fifty-three teleported
        // between two glints and the player read (0.0, 0.6, -1.6) on the way
        // through, with the entity set still holding the place he had left:
        // "the farthest marked node is 11524 metres out". Two pins went onto
        // the map at (15.2, 1.0, -5.6) and (2.4, 1.9, -5.7), which is nowhere.
        // Real play coordinates in this game run to thousands of units, so a
        // player within a hundred of the absolute origin is a placeholder.
        if (std::fabs(pp.x) + std::fabs(pp.z) < 100.0f)
        {
            if (g_loadingLogsLeft > 0)
            {
                --g_loadingLogsLeft;
                GS_LOG("[auto] the player reads (%.1f, %.1f, %.1f), which is the placeholder the game "
                       "uses while a world loads; nothing is pinned until he is somewhere",
                       pp.x, pp.y, pp.z);
            }
            g_heldSinceMs = 0;
            return;
        }

        // The node the crosshair is on: of every marked node the game has
        // loaded, the one whose bearing from the camera is nearest the view's,
        // within fifteen degrees, and at least three metres from the player so
        // that something underfoot cannot take it. Held for a second, it gets
        // one pin at its own position and nothing else does.
        //
        // Every marked node, not a nearest handful. Session fifty asked for
        // the sixteen nearest and then chose among those, and the log says
        // what that cost: twenty-seven marked nodes loaded, sixteen in reach,
        // and the pin went to a berry bush twenty-two metres out. The eleven
        // it dropped were the eleven farthest, which is where the glint was.
        // A bearing test wants the far ones most: a bush three metres away
        // sitting half a metre off the line is nine degrees wide, while a
        // glint a hundred metres out has to be within a metre to read as one.
        //
        // Heights are not in this: session forty had firewood a metre away
        // reading six metres below the player's feet, so an angle measured in
        // three axes is unusable. A bearing is not.
        gs::actors::Entity around[8];
        float angles[8];
        const float cap = gs::Settings::Get().radius;
        int n = 0;
        int marked = 0;
        gs::aim::Held held;
        View v;
        if (ViewRay(pp, &v))
        {
            const float flen = std::sqrt(v.fx * v.fx + v.fz * v.fz);
            if (flen > 1e-3f)
            {
                n = gs::actors::MarkedOnBearing(pp.x, pp.z, v.ox, v.oz, v.fx / flen, v.fz / flen,
                                                cap > 0.0f ? cap : 1.0e9f, 0.26f, 3.0f,
                                                around, angles, 8, &marked);
                // Once a session, on the first press: the whole gimmick set,
                // named, so the log says whether the glint was in it.
                if (g_setListingsLeft > 0 && now - g_flashOnMs > 700)
                {
                    --g_setListingsLeft;
                    gs::actors::LogGimmicks(pp.x, pp.z, v.ox, v.oz, v.fx / flen, v.fz / flen);
                }
                if (now - g_flashOnMs > 700) Calibrate(pp);
                // And what the game's own detect system is holding. This is
                // the game's own answer to the question the bearing can only
                // guess at, so it is asked on every press and it outranks the
                // guess when it lands on the crosshair.
                gs::aim::Eye eye;
                eye.px = pp.x; eye.py = pp.y; eye.pz = pp.z;
                eye.lx = pp.lx; eye.ly = pp.ly; eye.lz = pp.lz;
                eye.ox = v.ox; eye.oz = v.oz;
                eye.ux = v.fx / flen; eye.uz = v.fz / flen;
                const bool sayTargets = g_targetLogsLeft > 0 && now - g_targetLastMs > 1500;
                if (sayTargets) { --g_targetLogsLeft; g_targetLastMs = now; }
                held = gs::aim::DescribeTargets(eye, sayTargets);
            }
        }
        int pick = n > 0 ? 0 : -1;
        float pickAngle = n > 0 ? angles[0] : 0.0f;

        // The game's own detect target, when it has one on the crosshair,
        // beats anything the bearing found. Fifteen degrees is the same cone
        // the nodes are held to, and a target has to be somewhere between five
        // metres and half a kilometre to be a thing the player is looking at.
        //
        // Build 0.6.0 threw this route away for placing a pin "104 units off"
        // in session nineteen. The arithmetic in aim.h says that pin was a
        // hundred and four metres from the player because the target was, and
        // session forty-seven measured a real glint at a hundred and eighteen.
        // Being far away was the evidence against it, and being far away is
        // the whole point.
        // The detect system does not get the pin. Session fifty-three ran the
        // control nobody had run: Seth aimed at one glint, teleported, and
        // aimed at another. The actor the detect component was holding moved
        // around on its own and sat a hundred and forty-seven degrees off his
        // crosshair, then sixty-eight. It follows the scene and not the aim.
        //
        // The disassembly says why. Every field this route ever read lives
        // past the end of the object it was read from: the task is 112 bytes
        // and the reads were at +0x500 and beyond, the special mode component
        // is 248 and the scan ran to 2048, and the detect component's own code
        // stops at +0x250 while the reads were at +0x508 and +0x3E8. All of it
        // was the neighbouring allocation.
        //
        // It is still read, inside the real bounds now, and still logged, so a
        // build where something real turns up there would say so.
        const bool byTarget = false;

        // One chosen thing, whichever route named it, so the hold and the pin
        // below do not care which one did.
        struct Chosen
        {
            float x = 0, y = 0, z = 0;
            float angleDeg = 0;
            uint32_t eid = 0;
            const char* how = "";
            char name[64]{};
            bool valid = false;
        } chosen;
        if (byTarget)
        {
            chosen.x = held.x; chosen.y = held.y; chosen.z = held.z;
            chosen.angleDeg = held.angle;
            chosen.eid = held.eid;
            chosen.how = "the target the game's own detect system is holding";
            _snprintf_s(chosen.name, sizeof(chosen.name), _TRUNCATE, "%s at %s+0x%llX",
                        held.cls[0] == '.' ? held.cls + 4 : held.cls, held.where,
                        static_cast<unsigned long long>(held.at));
            chosen.valid = true;
        }
        else if (pick >= 0)
        {
            chosen.x = around[pick].x; chosen.y = around[pick].y; chosen.z = around[pick].z;
            chosen.angleDeg = angles[pick] * 57.2958f;
            chosen.eid = around[pick].eid;
            chosen.how = around[pick].how ? around[pick].how : "?";
            strncpy_s(chosen.name, sizeof(chosen.name),
                      around[pick].name[0] ? around[pick].name : "?", _TRUNCATE);
            chosen.valid = true;
        }
        if (byTarget && g_heldWinsLeft > 0)
        {
            --g_heldWinsLeft;
            GS_LOG("[auto] the game says it is holding %s, %.1f metres away, %.1f degrees off the "
                   "crosshair; that outranks %s",
                   chosen.name, held.dist, held.angle,
                   pick >= 0 ? "the node the bearing found" : "an empty bearing search");
        }

        if (g_autoLogsLeft > 0 && now - g_autoLastLogMs > 2000)
        {
            g_autoLastLogMs = now;
            --g_autoLogsLeft;
            GS_LOG("[auto] flash on at (%.1f, %.1f, %.1f); %d marked nodes loaded, %d candidate node(s) in radius, "
                   "%d within fifteen degrees of the crosshair; the farthest marked node is %.0f metres out",
                   pp.x, pp.y, pp.z, gs::actors::PickupCount(), marked, n,
                   gs::actors::MarkedReach(pp.x, pp.z));
            for (int i = 0; i < n && i < 5; ++i)
            {
                const float dx = around[i].x - pp.x, dz = around[i].z - pp.z;
                GS_LOG("[auto]   %s\"%s\" eid %08X %.1f degrees off, %.1f away at (%.1f, %.1f, %.1f)",
                       i == pick ? "ON THE CROSSHAIR " : "",
                       around[i].name[0] ? around[i].name : "?", around[i].eid, angles[i] * 57.2958f,
                       std::sqrt(dx * dx + dz * dz), around[i].x, around[i].y, around[i].z);
            }
            if (pick < 0) GS_LOG("[auto]   no node within fifteen degrees of the crosshair");
            if (held.valid)
                GS_LOG("[auto]   the game's detect system holds %s at %.1f metres, %.1f degrees off, %s%s",
                       held.cls[0] == '.' ? held.cls + 4 : held.cls, held.dist, held.angle,
                       held.inPools ? "live in the pools" : "not in the pools",
                       byTarget ? ", and it takes the pick" : ", so the bearing keeps the pick");
            else GS_LOG("[auto]   the game's detect system is holding nothing the mod can resolve");
        }

        // One pin, on the place the crosshair held for a second. The hold
        // is by position and not by object: session forty-six aimed at a
        // patch of berry bushes and the nearest by bearing swapped between
        // neighbours every pass, so a hold keyed on the object's id never
        // reached a second.
        if (!chosen.valid)
        {
            g_heldEid = 0;
            g_heldSinceMs = 0;
        }
        else
        {
            const float hx = chosen.x - g_heldX, hz = chosen.z - g_heldZ;
            const bool samePlace = g_heldSinceMs && std::sqrt(hx * hx + hz * hz) <= 6.0f;
            if (!samePlace) g_heldSinceMs = now;
            g_heldEid = chosen.eid;
            g_heldX = chosen.x;
            g_heldZ = chosen.z;
        }
        // Half a kilometre is past anything the crosshair can pick out, and a
        // node that resolves farther than that resolved in the wrong frame.
        const float chosenDist = chosen.valid
            ? std::sqrt((chosen.x - pp.x) * (chosen.x - pp.x) + (chosen.z - pp.z) * (chosen.z - pp.z))
            : 0.0f;
        if (chosen.valid && chosenDist > 500.0f)
        {
            GS_LOG("[auto] \"%s\" resolved %0.f metres away, which is too far to be what the crosshair "
                   "is on; not pinned", chosen.name, chosenDist);
            chosen.valid = false;
        }
        if (chosen.valid && g_heldSinceMs && now - g_heldSinceMs >= 1000 && now >= g_cooldownUntil &&
            !gs::mapicon::PinNear(chosen.x, chosen.z, 8.0f))
        {
            g_cooldownUntil = now + 3000;
            const float dx = chosen.x - pp.x, dz = chosen.z - pp.z;
            GS_LOG("[auto] the crosshair held \"%s\" eid %08X for a second, %.1f degrees off, %.1f metres away; pinning it where it stands",
                   chosen.name, chosen.eid, chosen.angleDeg, std::sqrt(dx * dx + dz * dz));
            {
                const float north = chosen.z - pp.z, east = chosen.x - pp.x;
                GS_LOG("[auto]   its position came from %s; that is %.0f metres %s and %.0f metres %s of you",
                       chosen.how, std::fabs(north), north >= 0 ? "north" : "south",
                       std::fabs(east), east >= 0 ? "east" : "west");
            }
            PlaceAt(chosen.x, chosen.y, chosen.z,
                    byTarget ? "automatic, what the game's detect system is holding"
                             : "automatic, the node under the crosshair",
                    "Glint", pp, 8.0f);
        }

        // The measurement, once the flash has been on for a moment.
        if (g_huntLeft > 0 && n > 0 && !g_huntN && now - g_flashOnMs > 1200 && now - g_huntLastMs > 10000)
        {
            g_huntLastMs = now;
            --g_huntLeft;
            g_huntN = n < 4 ? n : 4;
            for (int i = 0; i < g_huntN; ++i)
            {
                g_huntEnts[i] = around[i].ptr;
                g_huntEids[i] = around[i].eid;
                strncpy_s(g_huntNames[i], sizeof(g_huntNames[i]), around[i].name[0] ? around[i].name : "?", _TRUNCATE);
                g_huntOn[i] = gs::dump::EffectActivity(around[i].ptr, 0x400);
                char tag[24];
                _snprintf_s(tag, sizeof(tag), _TRUNCATE, "on%d", i + 1);
                GS_LOG("[hunt] \"%s\" eid %08X with the flash on, %d effects", g_huntNames[i], g_huntEids[i], g_huntOn[i]);
                gs::dump::GimmickState(tag, around[i].ptr, 0x480);
            }
            GS_LOG("[hunt] hold the flash on the one that glows, then let it end; the same four are read again");
        }
    }
}

// Called from the thunk every tick with the minimap root in rcx. Runs on the
// game's UI thread; keep it cheap and never let anything escape.
extern "C" void gs_OnMinimapTick(void* self)
{
    const uint64_t n = ++g_count;
    if (n == 1)
    {
        g_thread.store(GetCurrentThreadId());
        GS_LOG_OK("[tick] first minimap update on thread %lu, root 0x%p; forwarding to 0x%p",
                  GetCurrentThreadId(), self, gs_minimapOriginal);
    }
    if (g_probe.load() && (n % kSampleTicks) == 0) Probe(self);

    // Every quarter second, give the automatic marker a look. The entity
    // set is refreshed on the key thread; session twenty-one's stutter was
    // that walk running here.
    if (n - g_lastRefreshTick >= 15)
    {
        g_lastRefreshTick = n;
        FlushPending();
        AutoMark(GetTickCount());

        // The camera object moves; the probe follows it.
        static uintptr_t watchedCam = 0;
        const uintptr_t cam = gs::camera::This();
        if (cam != watchedCam)
        {
            if (watchedCam) gs::tick::DropProbe(reinterpret_cast<void*>(watchedCam));
            if (cam) gs::tick::AddProbe("camera", reinterpret_cast<void*>(cam), 0x400);
            watchedCam = cam;
        }
    }

    // A mark asked for elsewhere lands here, on the thread that owns icons.
    // The pin lands where the view ray goes: the first object on it, or the
    // ground under it when the camera is looking down. Never at the player.
    if (g_markPending.exchange(false))
    {
        const gs::player::Pos pp = gs::player::Read();
        if (!pp.valid || !gs::player::DetectComponent())
        {
            GS_LOG_ERR("[mark] NOT READY: %s. Wait for 'READY' in this log, then press again.",
                       !pp.valid ? "the player has not been located yet" : "the detect component has not been found yet");
            return;
        }
        const bool flash = gs::aim::FlashActive();
        GS_LOG("[mark] requested. player world (%.3f, %.3f, %.3f) local (%.3f, %.3f, %.3f), flash %s",
               pp.x, pp.y, pp.z, pp.lx, pp.ly, pp.lz, flash ? "on" : "off");

        float tx = 0, ty = 0, tz = 0;
        bool have = false;
        const char* how = "";

        // The game's own aim field, logged when present. Populated in session
        // seventeen and zero in eighteen and nineteen, so it is a hint, not
        // the aim.
        float a[3];
        if (gs::aim::AimPointLocal(gs::player::CharacterControlComponent(), pp.lx, pp.ly, pp.lz, a))
            GS_LOG("[mark] game aim field local (%.2f, %.2f, %.2f) -> world (%.2f, %.2f, %.2f)",
                   a[0], a[1], a[2], a[0] + pp.ox, a[1] + pp.oy, a[2] + pp.oz);

        gs::camera::LogAtPress(2.0f * std::atan2(pp.q[1], pp.q[3]));

        View v;
        if (!gs::actors::Ready())
            GS_LOG("[mark] actor manager not located yet, no ray");
        else if (gs::actors::Count() == 0)
            GS_LOG("[mark] actor set is empty, no ray");
        else if (!ViewRay(pp, &v))
            GS_LOG("[mark] facing quaternion (%.3f, %.3f, %.3f, %.3f) is not a yaw and no camera, no ray",
                   pp.q[0], pp.q[1], pp.q[2], pp.q[3]);
        else
        {
            // A real ray against the world's collision, from the camera along
            // the view. Session thirty: layer 0 hit a vertical face 74 units
            // out that was not the ground Seth pointed at. So every press
            // casts on every candidate layer word, with the flag both ways,
            // and logs each hit; a ground-like hit (normal pointing up) is
            // preferred, then the farthest hit. A hit nearer than the pivot
            // is the player's own body and the ray is cast again from just
            // past the pivot.
            const gs::camera::Pose cam = gs::camera::Read();
            const float camDist = (cam.valid && cam.dist > 0.5f && cam.dist < 30.0f) ? cam.dist : 6.0f;
            gs::physics::LogState();
            const float origin[3] = {v.ox, v.oy, v.oz};
            const float dir[3] = {v.fx, v.fy, v.fz};
            gs::physics::Hit best;
            int usedLayer = -1;
            float bestScore = -1e9f;
            const int layers[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 58,
                                  0x3B, 0x3C, 0x3D, 0x3E, 0x40, 0x44, 0x48};
            for (int fl = 0; fl < 2; ++fl)
            for (int li = 0; li < static_cast<int>(sizeof(layers) / sizeof(layers[0])); ++li)
            {
                const int layer = layers[li];
                const bool flag = fl == 1;
                gs::physics::Hit h = gs::physics::Cast(origin, dir, 600.0f, layer, flag);
                if (h.hit && h.dist < camDist + 1.0f)
                {
                    const float o2[3] = {v.ox + v.fx * (camDist + 1.0f), v.oy + v.fy * (camDist + 1.0f), v.oz + v.fz * (camDist + 1.0f)};
                    gs::physics::Hit h2 = gs::physics::Cast(o2, dir, 600.0f, layer, flag);
                    if (h2.hit) { h2.dist += camDist + 1.0f; h = h2; }
                    else h.hit = false;
                }
                if (!h.hit) continue;
                GS_LOG("[mark] layer %d flag %d: hit at %.1f, normal (%.2f, %.2f, %.2f), out flag %d, lands (%.1f, %.1f, %.1f)",
                       layer, flag ? 1 : 0, h.dist, h.normal[0], h.normal[1], h.normal[2], h.flag ? 1 : 0,
                       v.ox + v.fx * h.dist, v.oy + v.fy * h.dist, v.oz + v.fz * h.dist);
                // Sessions thirty and thirty-one: every direct hit sat on a face
                // of a 160 by 160 box around the player with a normal along
                // an axis. That is the edge of the loaded collision, not a
                // surface, and it does not count.
                const bool edge = std::fabs(h.normal[1]) < 0.05f &&
                                  (std::fabs(std::fabs(h.normal[0]) - 1.0f) < 0.01f || std::fabs(std::fabs(h.normal[2]) - 1.0f) < 0.01f);
                if (edge) continue;
                // Ground-like first, then far.
                const float score = (h.normal[1] > 0.3f ? 10000.0f : 0.0f) + h.dist;
                if (score > bestScore) { bestScore = score; best = h; usedLayer = layer; }
            }
            static int knownLayer = -1;

            // Sessions thirty and thirty-one: the direct ray stops at a face
            // square to it, at the same z plane both times, on every layer
            // that hits at all. So the ground is found another way: probes
            // straight down along the view, each from well above the ray,
            // and the ground point is where the terrain first rises to meet
            // the ray. A probe that finds nothing says collision is not
            // loaded there, which is its own answer.
            float probeT = -1, probeY = 0;
            {
                const float down[3] = {0, -1, 0};
                int misses = 0, probes = 0;
                float lastLoggedT = -100;
                float t1 = -1, h1 = 0, t2 = -1, h2 = 0;   // the last two probes that found ground
                for (float t = 4.0f; t <= 400.0f && probeT < 0; t += (t < 60.0f ? 4.0f : 8.0f))
                {
                    const float px = v.ox + v.fx * t, pz = v.oz + v.fz * t, rayY = v.oy + v.fy * t;
                    const float from[3] = {px, rayY + 150.0f, pz};
                    const gs::physics::Hit h = gs::physics::Cast(from, down, 900.0f, 0, false);
                    ++probes;
                    if (!h.hit) { ++misses; if (t - lastLoggedT >= 40.0f) { lastLoggedT = t; GS_LOG("[mark]   probe %.0f out: nothing below", t); } continue; }
                    const float groundY = from[1] - h.dist;
                    t1 = t2; h1 = h2; t2 = t; h2 = groundY;
                    if (t - lastLoggedT >= 40.0f || groundY >= rayY - 0.2f)
                    {
                        lastLoggedT = t;
                        GS_LOG("[mark]   probe %.0f out: ground at %.1f, ray at %.1f, normal (%.2f, %.2f, %.2f)", t, groundY, rayY,
                               h.normal[0], h.normal[1], h.normal[2]);
                    }
                    if (groundY >= rayY - 0.2f) { probeT = t; probeY = groundY; }
                }
                GS_LOG("[mark] %d probes, %d found nothing below; %s", probes, misses,
                       probeT > 0 ? "the ground meets the view ray" : "the view ray never meets the ground where collision is loaded");
                // Beyond the loaded window, carry the last slope forward until it
                // meets the ray. Wrong on a hill, right on a gentle fall, and
                // better than the window's edge.
                if (probeT < 0 && t1 > 0 && t2 > t1)
                {
                    const float slope = (h2 - h1) / (t2 - t1);
                    const float denom = v.fy - slope;
                    if (denom < -1e-4f)
                    {
                        const float t = (h2 - slope * t2 - v.oy) / denom;
                        if (t > t2 && t <= 500.0f)
                        {
                            probeT = t;
                            probeY = h2 + slope * (t - t2);
                            GS_LOG("[mark] beyond the window: the ground slope %.3f from the last probes (%.0f: %.1f, %.0f: %.1f) meets the ray %.0f units out at height %.1f (extrapolated)",
                                   slope, t1, h1, t2, h2, t, probeY);
                        }
                        else GS_LOG("[mark] beyond the window: the slope meets the ray at %.0f, out of range", t);
                    }
                    else GS_LOG("[mark] beyond the window: the ground falls away faster than the ray; no meeting point");
                }
            }
            if (probeT > 0)
            {
                best.hit = true;
                best.dist = probeT;
                usedLayer = knownLayer >= 0 ? knownLayer : 0;
                GS_LOG("[mark] the probes put the ground point %.0f units out at height %.1f; the direct ray said %s %.1f",
                       probeT, probeY, bestScore > -1e8f ? "a hit at" : "nothing", bestScore > -1e8f ? best.dist : 0.0f);
            }
            if (best.hit)
            {
                knownLayer = usedLayer;
                tx = v.ox + v.fx * best.dist; ty = v.oy + v.fy * best.dist; tz = v.oz + v.fz * best.dist;
                have = true;
                how = "the world ray";
                GS_LOG("[mark] chosen: layer %d, the world ray lands %.1f units out at (%.1f, %.1f, %.1f)", knownLayer, best.dist, tx, ty, tz);
            }
            else
            {
                GS_LOG("[mark] the world ray hit nothing on any layer; falling back to the terrain estimate");
                float gx = 0, gy = 0, gz = 0, gt = 0;
                uint32_t geid = 0;
                bool ground = v.camera && gs::nearest::GroundAlong(v.ox, v.oy, v.oz, v.fx, v.fy, v.fz, 300.0f,
                                                                    &gx, &gy, &gz, &gt, &geid);
                if (!ground) ground = GroundPoint(v, &gx, &gy, &gz, &gt);
                if (ground)
                {
                    have = true; tx = gx; ty = gy; tz = gz;
                    how = "the terrain estimate under the view ray";
                }
                else GS_LOG("[mark] no ground point either");
            }
        }

        // The detect component's scalars, kept in the log for the record.
        {
            const uintptr_t d = gs::player::DetectComponent();
            if (d && gs::rtti::Readable(reinterpret_cast<const void*>(d), 0x650))
            {
                const auto* q = reinterpret_cast<const uint8_t*>(d);
                float f3ec, f580, f300;
                uint32_t u410, u42c;
                memcpy(&f3ec, q + 0x3EC, 4); memcpy(&f580, q + 0x580, 4); memcpy(&f300, q + 0x300, 4);
                memcpy(&u410, q + 0x410, 4); memcpy(&u42c, q + 0x42C, 4);
                GS_LOG("[mark] detect scalars: +300 %.3f +3EC %.3f +580 %.3f +410 0x%X +42C 0x%X", f300, f3ec, f580, u410, u42c);
            }
        }

        if (have) PlaceAt(tx, ty, tz, how, "Mark", pp, 2.0f);
        else GS_LOG("[mark] no target resolved, nothing placed");
        (void)g_markX; (void)g_markZ; (void)g_markLabel;
    }
}

namespace gs::tick
{
    bool Install(uintptr_t miniVtable)
    {
        if (!miniVtable || g_swap.installed) return false;
        if (!vtable::Install(miniVtable, sig::kSlotUpdate,
                             reinterpret_cast<void*>(&gs_MinimapTickThunk), g_swap))
        {
            GS_LOG_ERR("[tick] could not take slot %d on the minimap vtable", sig::kSlotUpdate);
            return false;
        }
        gs_minimapOriginal = g_swap.original;
        GS_LOG_OK("[tick] slot %d on minimap vtable 0x%p was 0x%p, now the thunk; it tail-jumps there",
                  sig::kSlotUpdate, reinterpret_cast<void*>(miniVtable), g_swap.original);
        return true;
    }

    void Remove()
    {
        if (!g_swap.installed) return;
        bool leftAlone = false;
        if (vtable::Restore(g_swap, leftAlone)) GS_LOG("[tick] slot restored");
        else if (leftAlone) GS_LOG("[tick] slot now holds someone else's hook, left in place");
    }

    uint64_t Count() { return g_count.load(); }
    uint32_t ThreadId() { return g_thread.load(); }
    void SetProbe(bool on) { g_probe.store(on); }

    void AddProbe(const char* label, void* object, size_t bytes)
    {
        for (Extra& e : g_extras)
        {
            if (e.object.load() == object) return;
        }
        for (Extra& e : g_extras)
        {
            if (e.object.load()) continue;
            strncpy_s(e.label, sizeof(e.label), label ? label : "extra", _TRUNCATE);
            e.bytes = bytes > kExtraMax ? kExtraMax : bytes;
            e.havePrev = false;
            e.object.store(object);
            GS_LOG("[probe] watching %s at 0x%p, %zu bytes", e.label, object, e.bytes);
            return;
        }
        GS_LOG("[probe] no free slot for %s", label ? label : "extra");
    }

    void DropProbe(void* object)
    {
        for (Extra& e : g_extras)
            if (e.object.load() == object) e.object.store(nullptr);
    }

    void RequestMark(float x, float z, const char* label)
    {
        g_markX = x;
        g_markZ = z;
        strncpy_s(g_markLabel, sizeof(g_markLabel), label ? label : "GlintSpotter", _TRUNCATE);
        g_markPending.store(true);
    }

    void SetWorldRoot(void* root)
    {
        // Kept for the log. No call is ever made on it; see PlaceAt.
        g_worldRoot.store(root);
    }
}
