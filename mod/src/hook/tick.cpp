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
    struct Pending { float x, z; char label[16]; };
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
            gs::mapicon::PlacePinNow(root, g_pending[i].x, g_pending[i].z, g_pending[i].label);
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
                p.x = tx; p.z = tz;
                strncpy_s(p.label, sizeof(p.label), label, _TRUNCATE);
                GS_LOG("[mark] the world map has not been opened this session, so its root does not exist yet; "
                       "pin queued (%d waiting). Open the map once and it appears.", g_pendingN);
            }
            else GS_LOG_ERR("[mark] %d pins already waiting for the map to be opened; this one is dropped", g_pendingN);
            return;
        }
        gs::mapicon::PlacePinNow(root, tx, tz, label);
    }

    // The view ray. The camera's own forward when its object is in hand,
    // otherwise the body's facing held level. The origin is eye height.
    struct View
    {
        float ox = 0, oy = 0, oz = 0;
        float fx = 0, fy = 0, fz = 1;
        bool camera = false;
    };

    bool ViewRay(const gs::player::Pos& pp, View* v)
    {
        v->ox = pp.x; v->oy = pp.y + 1.6f; v->oz = pp.z;
        const gs::camera::Pose cam = gs::camera::Read();
        if (cam.fwdValid)
        {
            v->fx = cam.fwd[0]; v->fy = cam.fwd[1]; v->fz = cam.fwd[2];
            v->camera = true;
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
                 gs::nearest::Candidate* c, int n, gs::nearest::Candidate* miss, int missN)
    {
        if (v.camera)
            return gs::nearest::Cast3D(gs::player::Actor(), v.ox, v.oy, v.oz, v.fx, v.fy, v.fz,
                                       maxAlong, radius, spread, glintOnly, c, n, miss, missN);
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

    // Flash on: the object under the crosshair, the one nearest the view
    // ray by angle within six degrees, held for a second and a half, gets a
    // Glint pin once per area, and no other automatic pin for five seconds.
    //
    // No glint test yet. Session twenty-six showed the glint Seth aims at
    // is a character, not a gimmick, so the detect mode target byte on the
    // gimmick component cannot see it. The object's components are dumped
    // when the pin is placed with the flash on, and once more after the
    // flash ends, so the state that means "being revealed" can be read off
    // the diff and become the test.
    uint32_t g_autoEid = 0;
    uint32_t g_autoSinceMs = 0;
    uint32_t g_autoCooldownUntil = 0;
    int g_autoLogsLeft = 40;
    uint32_t g_autoLastLogMs = 0;
    uintptr_t g_autoDumpEntity = 0;   // dumped again once the flash is off
    uint32_t g_autoDumpEid = 0;
    uint32_t g_autoFlashOffMs = 0;
    int g_autoDumpsLeft = 3;

    int PickByAngle(const gs::nearest::Candidate* c, int n, float maxAngle)
    {
        int pick = -1;
        float best = maxAngle;
        for (int i = 0; i < n; ++i)
        {
            const float angle = c[i].off / (c[i].along > 1.0f ? c[i].along : 1.0f);
            if (angle < best) { best = angle; pick = i; }
        }
        return pick;
    }

    void AutoMark(uint32_t now)
    {
        if (!gs::aim::FlashActive())
        {
            g_autoEid = 0;
            g_autoSinceMs = 0;
            // The second dump, two seconds after the flash ended.
            if (g_autoDumpEntity)
            {
                if (!g_autoFlashOffMs) g_autoFlashOffMs = now;
                else if (now - g_autoFlashOffMs > 2000)
                {
                    GS_LOG("[auto] flash off for two seconds; dumping eid %08X again", g_autoDumpEid);
                    gs::dump::EntityComponents("off", g_autoDumpEntity, 0x200);
                    g_autoDumpEntity = 0;
                    g_autoFlashOffMs = 0;
                }
            }
            return;
        }
        g_autoFlashOffMs = 0;
        const gs::player::Pos pp = gs::player::Read();
        View v;
        if (!pp.valid || !ViewRay(pp, &v)) return;
        gs::nearest::Candidate c[8];
        const int n = CastView(v, 200.0f, 1.5f, 0.05f, false, c, 8, nullptr, 0);
        const int pick = PickByAngle(c, n, 0.07f);   // four degrees

        // While the flash is on, a line every two seconds saying what the
        // cone holds, forty lines at most.
        if (g_autoLogsLeft > 0 && now - g_autoLastLogMs > 2000)
        {
            g_autoLastLogMs = now;
            --g_autoLogsLeft;
            GS_LOG("[auto] flash on, view from the %s (pitch %.0f deg); set %d, %d gimmicks, %d with the byte, %d in the cone",
                   v.camera ? "camera" : "body, level", std::asin(v.fy) * 57.2958f,
                   gs::actors::Count(), gs::actors::GimmickCount(), gs::actors::GlintCount(), n);
            for (int i = 0; i < n && i < 4; ++i)
                GS_LOG("[auto]   %s%s%s%s eid %08X at %.1f along, %.2f off, %.1f deg, %+.1f up", i == pick ? "PICK " : "",
                       c[i].glint ? "BYTE " : "", c[i].gimmick ? "gimmick " : "", c[i].cls, c[i].eid, c[i].along, c[i].off,
                       std::atan2(c[i].off, c[i].along) * 57.2958f, c[i].dy);
        }

        if (pick < 0 || c[pick].eid == 0)
        {
            g_autoEid = 0;
            g_autoSinceMs = 0;
            return;
        }
        if (c[pick].eid != g_autoEid)
        {
            g_autoEid = c[pick].eid;
            g_autoSinceMs = now;
            return;
        }
        if (now - g_autoSinceMs < 1500 || now < g_autoCooldownUntil) return;
        g_autoCooldownUntil = now + 5000;
        GS_LOG("[auto] %s%s eid %08X held under the crosshair for a second and a half at %.1f units",
               c[pick].gimmick ? "gimmick " : "", c[pick].cls, c[pick].eid, c[pick].along);
        PlaceAt(c[pick].x, c[pick].y, c[pick].z, "automatic, held under the crosshair with the flash on", "Glint", pp, 8.0f);
        if (g_autoDumpsLeft > 0 && !g_autoDumpEntity)
        {
            --g_autoDumpsLeft;
            GS_LOG("[auto] dumping eid %08X with the flash on", c[pick].eid);
            gs::dump::EntityComponents("on", c[pick].entity, 0x200);
            g_autoDumpEntity = c[pick].entity;
            g_autoDumpEid = c[pick].eid;
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
            gs::nearest::Candidate c[6], miss[3];
            // 300 units out, a beam 1.5 units wide at the eye widening by 0.03
            // per unit, so about two degrees at the far end.
            const int n = CastView(v, 300.0f, 1.5f, 0.03f, false, c, 6, miss, 3);
            {
                int bands[5];
                float farthest = 0;
                gs::nearest::Reach(pp.x, pp.y, pp.z, bands, &farthest);
                GS_LOG("[mark] the set reaches %.0f units: %d within 30, %d to 60, %d to 120, %d to 300, %d beyond",
                       farthest, bands[0], bands[1], bands[2], bands[3], bands[4]);
            }
            GS_LOG("[mark] ray from the %s at (%.1f, %.1f, %.1f) along (%.3f, %.3f, %.3f) over %d entities: %d hit(s)",
                   v.camera ? "camera" : "body, level", v.ox, v.oy, v.oz, v.fx, v.fy, v.fz, gs::actors::Count(), n);
            for (int i = 0; i < n; ++i)
                GS_LOG("[mark]   %d. %s%s%s eid %08X at %.1f along, %.2f off, %+.1f up, (%.1f, %.1f, %.1f)",
                       i + 1, c[i].glint ? "GLINT " : "", c[i].gimmick ? "gimmick " : "", c[i].cls, c[i].eid,
                       c[i].along, c[i].off, c[i].dy, c[i].x, c[i].y, c[i].z);
            int missN = 0;
            for (int i = 0; i < 3 && miss[i].entity; ++i) ++missN;
            for (int i = 0; i < missN; ++i)
                GS_LOG("[mark]   near miss: %s%s eid %08X at %.1f along, %.2f off, %+.1f up",
                       miss[i].glint ? "GLINT " : "", miss[i].cls, miss[i].eid, miss[i].along, miss[i].off, miss[i].dy);
            for (int i = 0; i < n && i < 2; ++i) DescribeComponents(i == 0 ? "hit 1" : "hit 2", c[i].entity);
            if (missN > 0) DescribeComponents("near miss 1", miss[0].entity);

            {
                // "near" is a Windows macro; hence the name.
                gs::nearest::Candidate close[8];
                const int m = gs::nearest::Closest(gs::player::Actor(), pp.x, pp.y, pp.z, close, 8);
                for (int i = 0; i < m; ++i)
                    GS_LOG("[mark]   nearby %d: %s%s%s eid %08X %.1f away, %+.1f up, (%.1f, %.1f, %.1f)", i + 1,
                           close[i].glint ? "GLINT " : "", close[i].gimmick ? "gimmick " : "", close[i].cls, close[i].eid,
                           close[i].along, close[i].dy, close[i].x, close[i].y, close[i].z);
            }
            float gx = 0, gy = 0, gz = 0, gt = 0;
            uint32_t geid = 0;
            bool ground = v.camera && gs::nearest::GroundAlong(v.ox, v.oy, v.oz, v.fx, v.fy, v.fz, 300.0f,
                                                                &gx, &gy, &gz, &gt, &geid);
            if (ground)
                GS_LOG("[mark] the view ray meets the terrain %.1f units out at (%.1f, %.1f, %.1f), height from eid %08X",
                       gt, gx, gy, gz, geid);
            else
            {
                ground = GroundPoint(v, &gx, &gy, &gz, &gt);
                if (ground) GS_LOG("[mark] nothing stands near the ray to give a terrain height; the plane at the feet says %.1f units out at (%.1f, %.1f, %.1f)", gt, gx, gy, gz);
                else GS_LOG("[mark] no ground point: %s", v.camera ? "looking level or up" : "no camera pitch");
            }

            // The object nearest the crosshair by angle wins, within six
            // degrees; session twenty-six's aimed character sat four degrees
            // off at 32 units while a gimmick sat fourteen degrees off at 13
            // and would have won on distance. Otherwise the ground point.
            const int pick = PickByAngle(c, n, 0.07f);
            if (pick >= 0 && (!ground || c[pick].along <= gt + 3.0f))
            {
                have = true; tx = c[pick].x; ty = c[pick].y; tz = c[pick].z;
                how = "the object under the crosshair";
                GS_LOG("[mark] crosshair pick: hit %d, %.1f degrees off the view ray", pick + 1,
                       std::atan2(c[pick].off, c[pick].along) * 57.2958f);
            }
            else if (ground)
            {
                have = true; tx = gx; ty = gy; tz = gz;
                how = "the ground under the view ray";
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
