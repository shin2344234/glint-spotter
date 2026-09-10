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
    constexpr int kExtraSlots = 4;
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

    // A mark asked for elsewhere lands here, on the thread that owns icons.
    // The pin must land where the crosshair points, not where the player
    // stands. Until the aim is known, a request is logged with everything
    // this build does know, and nothing is placed.
    if (g_markPending.exchange(false))
    {
        const gs::player::Pos pp = gs::player::Read();
        GS_LOG("[mark] requested. player at (%.3f, %.3f, %.3f)%s; aim not known yet, nothing placed",
               pp.x, pp.y, pp.z, pp.valid ? "" : " (position walk not proven)");
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
