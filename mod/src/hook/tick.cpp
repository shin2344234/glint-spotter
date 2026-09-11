#include "hook/tick.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
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
    // The automatic marker's state. With the flash on and the detect
    // component reporting a target, the object at that distance along the
    // facing gets a pin once the target has held for a second.
    uint32_t g_autoSinceMs = 0;
    uint64_t g_lastRefreshTick = 0;

    void PlaceAt(float tx, float ty, float tz, const char* how, const char* label, const gs::player::Pos& pp, float dedupe)
    {
        if (gs::mapicon::PinNear(tx, tz, dedupe))
        {
            GS_LOG("[mark] a pin already sits within %.0f units of (%.1f, %.1f); not placing another", dedupe, tx, tz);
            return;
        }
        void* root = g_worldRoot.load();
        if (!root) root = gs::mapicon::LastWorldRoot();
        const float dx = tx - pp.x, dz = tz - pp.z;
        GS_LOG("[mark] target %.1f units away via %s; placing a %s pin there", std::sqrt(dx * dx + dz * dz), how, label);
        gs::mapicon::PlacePinNow(root, tx, tz, label);
        (void)ty;
    }

    // Flash on: the first gimmick in a narrow beam along the facing is the
    // candidate. When the same one stays the candidate for a second it gets a
    // Glint pin, once per area, and no other automatic pin for five seconds.
    // The detect component's own target field is still unknown (session
    // twenty read -1 with the flash on a glint), so the pick is geometric.
    uint32_t g_autoEid = 0;
    uint32_t g_autoCooldownUntil = 0;
    int g_autoLogsLeft = 40;
    uint32_t g_autoLastLogMs = 0;

    void AutoMark(uint32_t now)
    {
        if (!gs::aim::FlashActive())
        {
            g_autoEid = 0;
            g_autoSinceMs = 0;
            return;
        }
        const gs::player::Pos pp = gs::player::Read();
        float fx = 0, fz = 0;
        if (!pp.valid || !gs::nearest::ForwardFromQuat(pp.q, &fx, &fz)) return;
        gs::nearest::Candidate c[8];
        const int n = gs::nearest::Cast(gs::player::Actor(), pp.x, pp.y + 1.6f, pp.z, fx, fz, 40.0f, 1.5f, 0.08f, c, 8);

        // While the flash is on, a line every two seconds saying what sits in
        // the beam, so the glint's class and flags can be read off the log.
        if (g_autoLogsLeft > 0 && now - g_autoLastLogMs > 2000)
        {
            g_autoLastLogMs = now;
            --g_autoLogsLeft;
            float dd = 0;
            const bool hd = gs::aim::DetectDistance(&dd);
            GS_LOG("[auto] flash on, %d in the beam, set %d, detect distance %s %.2f", n, gs::actors::Count(), hd ? "is" : "none,", dd);
            for (int i = 0; i < n && i < 4; ++i)
                GS_LOG("[auto]   %s%s eid %08X at %.1f along, %.2f off, %+.1f up", c[i].gimmick ? "gimmick " : "", c[i].cls, c[i].eid, c[i].along, c[i].off, c[i].dy);
        }

        int pick = -1;
        for (int i = 0; i < n; ++i) if (c[i].gimmick) { pick = i; break; }
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
        if (now - g_autoSinceMs < 1000 || now < g_autoCooldownUntil) return;
        g_autoCooldownUntil = now + 5000;
        GS_LOG("[auto] %s eid %08X held in the beam for a second at %.1f units", c[pick].cls, c[pick].eid, c[pick].along);
        PlaceAt(c[pick].x, c[pick].y, c[pick].z, "automatic, gimmick held in the beam", "Glint", pp, 8.0f);
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
    // The pin lands where the flash is aimed: the actor the detect component
    // is focused on, read on this thread. Nothing is placed at the player.
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

        // Everything read off the actor is in the sub-level's local space, and
        // the map wants world space. The origin comes from the transform's own
        // two positions, so it is right for whatever sub-level this is.
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
        float detectDist = 0;
        const bool hasDetect = gs::aim::DetectDistance(&detectDist);
        GS_LOG("[mark] detect target: %s", hasDetect ? "present" : "none");

        // A ray from the player along the facing, against every entity in the
        // actor manager's list. Needs nothing the game only sets sometimes;
        // session eighteen had the aim field zeroed. Hits objects, not terrain.
        if (!have)
        {
            float fx = 0, fz = 0;
            if (!gs::actors::Ready())
                GS_LOG("[mark] actor manager not located yet, no ray");
            else if (gs::actors::Count() == 0)
                GS_LOG("[mark] actor set is empty, no ray");
            else if (!gs::nearest::ForwardFromQuat(pp.q, &fx, &fz))
                GS_LOG("[mark] facing quaternion (%.3f, %.3f, %.3f, %.3f) is not a yaw, no ray",
                       pp.q[0], pp.q[1], pp.q[2], pp.q[3]);
            else
            {
                gs::nearest::Candidate c[6];
                // 60 units out, a beam 1.5 units wide at the player widening by
                // 0.06 per unit, so 5 units wide at the far end.
                const int n = gs::nearest::Cast(gs::player::Actor(), pp.x, pp.y + 1.6f, pp.z, fx, fz,
                                                60.0f, 1.5f, 0.06f, c, 6);
                gs::camera::LogAtPress(2.0f * std::atan2(pp.q[1], pp.q[3]), pp.x, pp.y, pp.z, fx, fz);
                GS_LOG("[mark] ray from (%.1f, %.1f, %.1f) along (%.3f, %.3f) over %d entities: %d hit(s)",
                       pp.x, pp.y, pp.z, fx, fz, gs::actors::Count(), n);
                for (int i = 0; i < n; ++i)
                    GS_LOG("[mark]   %d. %s%s eid %08X at %.1f along, %.2f off, %+.1f up, (%.1f, %.1f, %.1f)",
                           i + 1, c[i].gimmick ? "gimmick " : "", c[i].cls, c[i].eid, c[i].along, c[i].off, c[i].dy, c[i].x, c[i].y, c[i].z);
                if (n > 0)
                {
                    have = true; tx = c[0].x; ty = c[0].y; tz = c[0].z;
                    how = "first object on the ray";
                }
            }
        }

        // Fallback: the detect distance along the facing, which is where the
        // game says its target is even when the ray saw nothing there.
        if (!have && hasDetect && detectDist > 0.5f && detectDist < 80.0f)
        {
            float fx = 0, fz = 0;
            if (gs::nearest::ForwardFromQuat(pp.q, &fx, &fz))
            {
                tx = pp.x + fx * detectDist; ty = pp.y; tz = pp.z + fz * detectDist;
                have = true;
                how = "detect distance along the facing";
            }
        }
        // The actor-pointer route found a fixed reference 104 units away at every
        // press in session nineteen. It is not an aim and it places nothing.

        // The detect component's scalars that moved when the flash was aimed
        // in session seventeen, next to the ray's candidates, so the one that
        // is the distance to the glint can be picked out. +0x580 went from
        // 12.6 to 3.0 between an unaimed and an aimed press.
        {
            const uintptr_t d = gs::player::DetectComponent();
            if (d && gs::rtti::Readable(reinterpret_cast<const void*>(d), 0x650))
            {
                const auto* q = reinterpret_cast<const uint8_t*>(d);
                float f3e8, f3ec, f580, f5e8, f640, f2ec, f300, f368;
                uint32_t u410, u42c;
                memcpy(&f3e8, q + 0x3E8, 4); memcpy(&f3ec, q + 0x3EC, 4); memcpy(&f580, q + 0x580, 4);
                memcpy(&f5e8, q + 0x5E8, 4); memcpy(&f640, q + 0x640, 4);
                memcpy(&f2ec, q + 0x2EC, 4); memcpy(&f300, q + 0x300, 4); memcpy(&f368, q + 0x368, 4);
                memcpy(&u410, q + 0x410, 4); memcpy(&u42c, q + 0x42C, 4);
                GS_LOG("[mark] detect scalars: +2EC %.3f +300 %.3f +368 %.3f +3E8 %.3f +3EC %.3f +580 %.3f +5E8 %.3f +640 %.3f +410 0x%X +42C 0x%X",
                       f2ec, f300, f368, f3e8, f3ec, f580, f5e8, f640, u410, u42c);
            }
        }

        if (have) PlaceAt(tx, ty, tz, how, "Mark", pp, 2.0f);
        else
        {
            GS_LOG("[mark] no target resolved, nothing placed");
        }
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

    void SetWorldRoot(void* root) { g_worldRoot.store(root); }
}
