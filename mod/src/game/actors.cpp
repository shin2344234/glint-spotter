#include "game/actors.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "game/rtti.h"
#include "game/typescan.h"

namespace
{
    // Master Looter's offsets, build 2.01.00.
    constexpr uintptr_t kOff_Mgr_PoolsBegin  = 0x100;
    constexpr uintptr_t kOff_Mgr_PoolsEnd    = 0x200;
    constexpr uintptr_t kOff_Ent_Eid         = 0x60;
    constexpr uintptr_t kOff_Ent_Comps       = 0x68;
    constexpr uintptr_t kOff_Comps_Transform = 0x1A0;
    constexpr uintptr_t kOff_Tf_WorldPos     = 0x29C;
    constexpr uint32_t  kPoolCapMax          = 0x10000;
    constexpr uint32_t  kKeepMs              = 12000;
    constexpr int       kSetMax              = 4096;

    std::atomic<uintptr_t> g_vtable{0};
    std::atomic<uintptr_t> g_slot{0};      // the global that holds the manager pointer
    std::atomic<uintptr_t> g_mgr{0};
    uint32_t g_foundAt = 0;
    bool g_rechecked = false;
    uintptr_t g_slots[16];
    int g_slotN = 0;
    bool g_slotsScanned = false;

    std::mutex g_setMutex;
    gs::actors::Entity g_set[kSetMax];
    int g_setN = 0;

    uintptr_t Deref(uintptr_t at)
    {
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 8)) return 0;
        return *reinterpret_cast<const uintptr_t*>(at);
    }

    // Every qword in the module image that points at an object whose first
    // qword is the manager's vtable. The image is a few hundred megabytes and
    // this runs once. Guarded: image sections are not all readable.
    int FindGlobals(uintptr_t vtable, uintptr_t* out, int cap)
    {
        uintptr_t base = 0;
        size_t size = 0;
        if (!gs::typescan::ModuleRange(base, size)) return 0;
        int n = 0;
        // Region by region, so a guard page or an unmapped section in the image
        // ends one region's walk and not the whole scan. Code holds no pointers
        // to heap objects, so the execute regions are skipped.
        uintptr_t at = base;
        while (at < base + size && n < cap)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi))) break;
            const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t re = rb + mbi.RegionSize;
            const DWORD data = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY;
            const bool walk = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && (mbi.Protect & data) != 0;
            if (walk)
            {
                __try
                {
                    const uintptr_t* p = reinterpret_cast<const uintptr_t*>((rb + 7) & ~uintptr_t(7));
                    const uintptr_t* end = reinterpret_cast<const uintptr_t*>(re);
                    for (; p + 1 <= end && n < cap; ++p)
                    {
                        const uintptr_t v = *p;
                        // A heap pointer: high, aligned, outside the image.
                        if (v < 0x10000 || (v & 7) != 0 || v > 0x00007FFFFFFFFFFFull ||
                            (v >= base && v < base + size)) continue;
                        if (!gs::rtti::Readable(reinterpret_cast<const void*>(v), 8)) continue;
                        if (*reinterpret_cast<const uintptr_t*>(v) != vtable) continue;
                        out[n++] = reinterpret_cast<uintptr_t>(p);
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            at = re;
        }
        return n;
    }

    // How many entities the pools of this manager hold right now.
    uint32_t Held(uintptr_t mgr)
    {
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(mgr), kOff_Mgr_PoolsEnd)) return 0;
            uint32_t held = 0;
            for (uintptr_t off = kOff_Mgr_PoolsBegin; off + 16 <= kOff_Mgr_PoolsEnd; off += 8)
            {
                const uint32_t count = *reinterpret_cast<const uint32_t*>(mgr + off);
                const uint32_t cap = *reinterpret_cast<const uint32_t*>(mgr + off + 4);
                const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(mgr + off + 8);
                if (count && cap && count <= cap && cap <= kPoolCapMax && arr) held += count;
            }
            return held;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    bool WorldPos(uintptr_t e, float* out)
    {
        __try
        {
            const uintptr_t comps = Deref(e + kOff_Ent_Comps);
            if (!comps) return false;
            const uintptr_t tf = Deref(comps + kOff_Comps_Transform);
            if (!tf || !gs::rtti::Readable(reinterpret_cast<const void*>(tf + kOff_Tf_WorldPos), 12)) return false;
            memcpy(out, reinterpret_cast<const void*>(tf + kOff_Tf_WorldPos), 12);
            if (!std::isfinite(out[0]) || !std::isfinite(out[1]) || !std::isfinite(out[2])) return false;
            if (std::fabs(out[0]) + std::fabs(out[1]) + std::fabs(out[2]) > 1.0e6f) return false;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // One pass over the pools into a flat buffer, guarded, no C++ objects.
    int ReadPools(uintptr_t mgr, uintptr_t* out, int cap)
    {
        int n = 0;
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(mgr), kOff_Mgr_PoolsEnd)) return 0;
            for (uintptr_t off = kOff_Mgr_PoolsBegin; off + 16 <= kOff_Mgr_PoolsEnd && n < cap; off += 8)
            {
                const uint32_t count = *reinterpret_cast<const uint32_t*>(mgr + off);
                const uint32_t pcap = *reinterpret_cast<const uint32_t*>(mgr + off + 4);
                const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(mgr + off + 8);
                if (!count || !pcap || count > pcap || pcap > kPoolCapMax || !arr) continue;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(arr), static_cast<size_t>(count) * 8)) continue;
                const uintptr_t* ents = reinterpret_cast<const uintptr_t*>(arr);
                for (uint32_t i = 0; i < count && n < cap; ++i)
                    if (ents[i]) out[n++] = ents[i];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return n;
    }

    uint32_t EidOf(uintptr_t e)
    {
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(e + kOff_Ent_Eid), 4)) return 0;
            return *reinterpret_cast<const uint32_t*>(e + kOff_Ent_Eid);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
}

namespace gs::actors
{
    void SetManagerVtable(uintptr_t vtable) { g_vtable.store(vtable); }

    bool Locate(uint32_t nowMs)
    {
        const uintptr_t vt = g_vtable.load();
        if (!vt) return false;

        // Once found, recheck a single time twenty seconds later: the first pick
        // is made before the world exists and every candidate reads zero then.
        if (g_mgr.load() && g_foundAt && !g_rechecked && nowMs - g_foundAt > 20000)
        {
            g_rechecked = true;
            g_mgr.store(0);
            GS_LOG("[actors] re-checking which manager holds the world now that one is loaded");
        }
        if (g_mgr.load()) return true;

        if (!g_slotsScanned)
        {
            g_slotsScanned = true;
            g_slotN = FindGlobals(vt, g_slots, 16);
            GS_LOG("[actors] %d global(s) in the image hold a ClientActorManager", g_slotN);
        }
        if (g_slotN == 0) return false;

        int best = -1;
        uintptr_t bestMgr = 0, bestSlot = 0;
        for (int i = 0; i < g_slotN; ++i)
        {
            const uintptr_t inst = Deref(g_slots[i]);
            if (!inst) continue;
            const int held = static_cast<int>(Held(inst));
            if (held > best) { best = held; bestMgr = inst; bestSlot = g_slots[i]; }
        }
        if (!bestMgr) return false;
        g_mgr.store(bestMgr);
        g_slot.store(bestSlot);
        if (!g_foundAt) g_foundAt = nowMs;
        GS_LOG_OK("[actors] manager 0x%p via global +0x%llX, %d entities in its pools",
                  reinterpret_cast<void*>(bestMgr),
                  static_cast<unsigned long long>(bestSlot - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
                  best);
        return true;
    }

    bool Ready() { return g_mgr.load() != 0; }
    uintptr_t Manager() { return g_mgr.load(); }

    uint32_t Refresh(uint32_t nowMs)
    {
        const uintptr_t mgr = g_mgr.load();
        if (!mgr) return 0;

        static uintptr_t buf[8192];
        const int n = ReadPools(mgr, buf, 8192);

        std::lock_guard<std::mutex> lock(g_setMutex);
        // Drop the stale.
        int w = 0;
        for (int i = 0; i < g_setN; ++i)
            if (nowMs - g_set[i].lastSeenMs <= kKeepMs) g_set[w++] = g_set[i];
        g_setN = w;

        for (int i = 0; i < n; ++i)
        {
            const uintptr_t e = buf[i];
            int j = 0;
            for (; j < g_setN; ++j) if (g_set[j].ptr == e) break;
            float pos[3];
            if (!WorldPos(e, pos)) continue;
            if (j == g_setN)
            {
                if (g_setN >= kSetMax) continue;
                g_set[g_setN].ptr = e;
                g_set[g_setN].eid = EidOf(e);
                ++g_setN;
            }
            g_set[j].x = pos[0]; g_set[j].y = pos[1]; g_set[j].z = pos[2];
            g_set[j].lastSeenMs = nowMs;
        }
        return static_cast<uint32_t>(n);
    }

    int Snapshot(Entity* out, int n)
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        const int c = g_setN < n ? g_setN : n;
        memcpy(out, g_set, static_cast<size_t>(c) * sizeof(Entity));
        return c;
    }

    int Count()
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        return g_setN;
    }

    uintptr_t ByEid(uint32_t eid)
    {
        if (!eid || eid == 0xFFFFFFFF) return 0;
        std::lock_guard<std::mutex> lock(g_setMutex);
        for (int i = 0; i < g_setN; ++i) if (g_set[i].eid == eid) return g_set[i].ptr;
        return 0;
    }
}
