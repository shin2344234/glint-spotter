#include "game/pickup.h"

#include <Windows.h>
#include <atomic>
#include <cstring>
#include <mutex>

#include "core/load.h"
#include "core/log.h"
#include "game/rtti.h"
#include "game/typescan.h"
#include "hook/vtable.h"

namespace
{
    constexpr uintptr_t kPickUpVtableRva = 0x05B34BB8;   // 2976
    constexpr const char* kPickUpClass = ".?AVTrocTrProcessPickUpItemOnceTimer@pa@@";
    constexpr int kHandlerSlot = 2;
    constexpr uintptr_t kOff_Context_Payload = 0x18;
    constexpr uintptr_t kOff_Payload_Item = 4;

    using HandlerFn = void* (*)(void*, void*, void*, void*);
    gs::vtable::Swap g_swap;
    HandlerFn g_orig = nullptr;

    std::mutex g_mutex;
    constexpr int kRing = 64;
    uint32_t g_ring[kRing];
    int g_head = 0, g_count = 0;
    std::atomic<uint32_t> g_seen{0};

    // Plain reads under a guard, kept out of the hook so the hook itself has
    // nothing that unwinds.
    uint32_t ItemOf(void* context)
    {
        __try
        {
            if (!context) return 0;
            const uintptr_t payload =
                *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(context) + kOff_Context_Payload);
            if (payload < 0x10000) return 0;
            return *reinterpret_cast<const uint32_t*>(payload + kOff_Payload_Item);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void Note(uint32_t eid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_ring[(g_head + g_count) % kRing] = eid;
        if (g_count < kRing) ++g_count;
        else g_head = (g_head + 1) % kRing;
    }

    double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
    {
        static LARGE_INTEGER f{};
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        return 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
    }

    // Timed, both halves: a hitch was felt around the sealed artifact's pick
    // up on 23 September, and this says whether any of it was the mod's part
    // or all of it the game's own handler.
    void* Handler(void* self, void* out, void* context, void* a4)
    {
        LARGE_INTEGER t0, t1, t2;
        QueryPerformanceCounter(&t0);
        const uint32_t eid = ItemOf(context);
        if (eid) Note(eid);
        QueryPerformanceCounter(&t1);
        gs::load::Book(gs::load::kPickUp, t1.QuadPart - t0.QuadPart);
        void* r = g_orig ? g_orig(self, out, context, a4) : out;
        QueryPerformanceCounter(&t2);
        const uint32_t n = ++g_seen;
        const double mine = Ms(t0, t1), game = Ms(t1, t2);
        if (n <= 40 || mine > 2.0 || game > 16.0)
            GS_LOG("[pickup] the game handled a pick up of eid %08X on thread %lu (%u this session): the mod's "
                   "part took %.3f ms, the game's own handler %.1f ms", eid, GetCurrentThreadId(), n, mine, game);
        return r;
    }
}

namespace gs::pickup
{
    bool Install()
    {
        if (g_swap.installed) return true;
        uintptr_t base = 0;
        size_t size = 0;
        if (!gs::typescan::ModuleRange(base, size)) return false;
        uintptr_t vt = base + kPickUpVtableRva;
        if (!gs::rtti::VtableIs(reinterpret_cast<const void*>(vt), kPickUpClass))
        {
            vt = 0;
            const char* kw[] = {kPickUpClass};
            static gs::typescan::ClassInfo found[4];
            const size_t n = gs::typescan::FindClasses(kw, 1, found, 4);
            for (size_t i = 0; i < n && !vt; ++i)
                if (found[i].vtableVa && strcmp(found[i].name, kPickUpClass) == 0) vt = found[i].vtableVa;
            if (!vt)
            {
                GS_LOG_ERR("[pickup] RTTI does not offer %s, so pickups are not heard and a pin on a sealed "
                           "artifact stays until you remove it", kPickUpClass);
                return false;
            }
            GS_LOG_OK("[pickup] the pick up message's vtable had moved; RTTI found it at +0x%llX",
                      static_cast<unsigned long long>(vt - base));
        }
        if (!gs::vtable::Install(vt, kHandlerSlot, reinterpret_cast<void*>(&Handler), g_swap))
        {
            GS_LOG_ERR("[pickup] slot %d of the pick up message's vtable would not take the hook", kHandlerSlot);
            return false;
        }
        g_orig = reinterpret_cast<HandlerFn>(g_swap.original);
        GS_LOG_OK("[pickup] listening for the game's pick up message, slot %d of +0x%llX, forwarding to 0x%p",
                  kHandlerSlot, static_cast<unsigned long long>(vt - base), g_swap.original);
        return true;
    }

    void Remove()
    {
        bool leftAlone = false;
        if (g_swap.installed) gs::vtable::Restore(g_swap, leftAlone);
    }

    int Take(uint32_t* out, int n)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        int got = 0;
        while (g_count > 0 && got < n)
        {
            out[got++] = g_ring[g_head];
            g_head = (g_head + 1) % kRing;
            --g_count;
        }
        return got;
    }

    uint32_t Seen() { return g_seen.load(); }
}
