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
    constexpr uintptr_t kOff_Mgr_ListsBegin  = 0x100;
    constexpr uintptr_t kOff_Mgr_ListsEnd    = 0x200;
    constexpr uintptr_t kOff_Mgr_PoolsBegin  = 0x100;
    constexpr uintptr_t kOff_Mgr_PoolsEnd    = 0x300;
    constexpr uintptr_t kOff_Ent_Eid         = 0x60;
    constexpr uintptr_t kOff_Ent_Comps       = 0x68;
    constexpr uintptr_t kOff_Comps_Transform = 0x1A0;
    constexpr uintptr_t kOff_Tf_WorldPos     = 0x29C;
    constexpr uintptr_t kComps_SlotsEnd      = 0x80;
    constexpr uint32_t  kListCapMax          = 0x10000;
    constexpr uint32_t  kKeepMs              = 12000;
    constexpr int       kSetMax              = 4096;
    constexpr int       kBufMax              = 16384;

    std::atomic<uintptr_t> g_vtable{0};
    std::atomic<uintptr_t> g_slot{0};      // the global that holds the manager pointer
    std::atomic<uintptr_t> g_mgr{0};
    uint32_t g_checkedAt = 0;
    uintptr_t g_slots[16];
    int g_slotN = 0;
    bool g_slotsScanned = false;

    std::mutex g_setMutex;
    gs::actors::Entity g_set[kSetMax];
    int g_setN = 0;
    uintptr_t g_buf[kBufMax];              // pool read scratch, tick thread only
    uint32_t g_lastSaidMs = 0;

    uintptr_t Deref(uintptr_t at)
    {
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 8)) return 0;
        return *reinterpret_cast<const uintptr_t*>(at);
    }

    // Every qword in the module's data sections that points at an object whose
    // first qword is the manager's vtable. Runs once.
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

    // An entity: readable, with an id whose top byte says player or world
    // object. Master Looter's EntityLike.
    //
    // No VirtualQuery per entry: a thousand of those every pass made the game
    // stutter in session twenty-one. The handler around the read is the guard.
    bool EntityLike(uintptr_t e)
    {
        __try
        {
            if (e < 0x10000 || (e & 7) != 0 || e > 0x00007FFFFFFFFFFFull) return false;
            const uint32_t id = *reinterpret_cast<const uint32_t*>(e + kOff_Ent_Eid);
            if (!id) return false;
            const uint8_t tag = static_cast<uint8_t>(id >> 24);
            return tag == 0xA0 || tag == 0xB0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // One pass over the manager into a flat buffer, guarded, no C++ objects.
    //
    // Two sources. The {count, capacity, array} triples between +0x100 and
    // +0x200 hold an entity or none on most ticks; session twenty read two.
    // The world sits in a dozen pools between +0x100 and +0x300, each a
    // pointer to an array of entity pointers with no usable count, so from
    // each pool whose first four entries are entities the walk runs forward
    // until sixteen entries in a row are not, and skips any pool inside a
    // run already covered. Master Looter's ForEachEntity, found on 2760.
    int ReadPools(uintptr_t mgr, uintptr_t* out, int cap)
    {
        int n = 0;
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(mgr), kOff_Mgr_PoolsEnd)) return 0;
            for (uintptr_t off = kOff_Mgr_ListsBegin; off + 16 <= kOff_Mgr_ListsEnd && n < cap; off += 8)
            {
                const uint32_t count = *reinterpret_cast<const uint32_t*>(mgr + off);
                const uint32_t lcap = *reinterpret_cast<const uint32_t*>(mgr + off + 4);
                const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(mgr + off + 8);
                if (!count || !lcap || count > lcap || lcap > kListCapMax || !arr) continue;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(arr), static_cast<size_t>(count) * 8)) continue;
                const uintptr_t* ents = reinterpret_cast<const uintptr_t*>(arr);
                for (uint32_t i = 0; i < count && i < 4000 && n < cap; ++i)
                    if (EntityLike(ents[i])) out[n++] = ents[i];
            }

            uintptr_t runs[64];
            int runN = 0;
            for (uintptr_t off = kOff_Mgr_PoolsBegin; off + 8 <= kOff_Mgr_PoolsEnd && runN < 64; off += 8)
            {
                const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(mgr + off);
                if (arr < 0x10000 || (arr & 7) != 0) continue;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(arr), 8 * 4)) continue;
                const uintptr_t* ents = reinterpret_cast<const uintptr_t*>(arr);
                bool ok = true;
                for (int i = 0; i < 4 && ok; ++i) ok = EntityLike(ents[i]);
                if (ok) runs[runN++] = arr;
            }
            // Ascending, so a pool that starts inside an earlier run is skipped.
            for (int i = 1; i < runN; ++i)
            {
                const uintptr_t v = runs[i];
                int j = i;
                while (j > 0 && runs[j - 1] > v) { runs[j] = runs[j - 1]; --j; }
                runs[j] = v;
            }
            uintptr_t coveredTo = 0;
            for (int r = 0; r < runN && n < cap; ++r)
            {
                if (runs[r] < coveredTo) continue;
                uintptr_t at = runs[r];
                int misses = 0;
                for (uint32_t i = 0; i < 8000 && n < cap; ++i, at += 8)
                {
                    const uintptr_t e = *reinterpret_cast<const uintptr_t*>(at);
                    if (!EntityLike(e)) { if (++misses >= 16) break; continue; }
                    misses = 0;
                    out[n++] = e;
                }
                coveredTo = at;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return n;
    }

    bool PtrLike(uintptr_t p) { return p >= 0x10000 && (p & 7) == 0 && p <= 0x00007FFFFFFFFFFFull; }

    bool WorldPos(uintptr_t e, float* out)
    {
        __try
        {
            const uintptr_t comps = *reinterpret_cast<const uintptr_t*>(e + kOff_Ent_Comps);
            if (!PtrLike(comps)) return false;
            const uintptr_t tf = *reinterpret_cast<const uintptr_t*>(comps + kOff_Comps_Transform);
            if (!PtrLike(tf)) return false;
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

    uint32_t EidOf(uintptr_t e)
    {
        __try
        {
            if (!PtrLike(e)) return 0;
            return *reinterpret_cast<const uint32_t*>(e + kOff_Ent_Eid);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // Does the entity's component block hold a ClientGimmickActorComponent?
    // A gimmick is what Master Looter reads identity from, and the glint effect
    // is named for it. Read once when the entity joins the set.
    bool HasGimmick(uintptr_t e)
    {
        __try
        {
            const uintptr_t comps = Deref(e + kOff_Ent_Comps);
            if (!comps || !gs::rtti::Readable(reinterpret_cast<const void*>(comps), kComps_SlotsEnd)) return false;
            for (uintptr_t off = 0; off < kComps_SlotsEnd; off += 8)
            {
                const uintptr_t c = *reinterpret_cast<const uintptr_t*>(comps + off);
                if (c < 0x10000 || (c & 7) != 0) continue;
                const uintptr_t vt = Deref(c);
                const char* n = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
                if (n && strstr(n, "ClientGimmickActorComponent")) return true;
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // How many entities a manager offers right now, for the pick.
    int Held(uintptr_t mgr)
    {
        static uintptr_t scratch[kBufMax];
        return ReadPools(mgr, scratch, kBufMax);
    }
}

namespace gs::actors
{
    void SetManagerVtable(uintptr_t vtable) { g_vtable.store(vtable); }

    bool Locate(uint32_t nowMs)
    {
        const uintptr_t vt = g_vtable.load();
        if (!vt) return false;

        // Keep the current pick while it offers a world. While it offers
        // nothing, look again every twenty seconds: the first pick is made
        // before the world exists and every candidate reads zero then.
        const uintptr_t cur = g_mgr.load();
        if (cur)
        {
            if (nowMs - g_checkedAt < 20000) return true;
            g_checkedAt = nowMs;
            if (Held(cur) > 0) return true;
            GS_LOG("[actors] manager 0x%p offers no entities; looking at the other globals", reinterpret_cast<void*>(cur));
        }

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
            const int held = Held(inst);
            if (held > best) { best = held; bestMgr = inst; bestSlot = g_slots[i]; }
        }
        if (!bestMgr) return false;
        g_checkedAt = nowMs;
        if (bestMgr != cur)
        {
            g_mgr.store(bestMgr);
            g_slot.store(bestSlot);
            GS_LOG_OK("[actors] manager 0x%p via global +0x%llX, %d entities in its pools",
                      reinterpret_cast<void*>(bestMgr),
                      static_cast<unsigned long long>(bestSlot - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
                      best);
        }
        return true;
    }

    bool Ready() { return g_mgr.load() != 0; }
    uintptr_t Manager() { return g_mgr.load(); }

    uint32_t Refresh(uint32_t nowMs)
    {
        const uintptr_t mgr = g_mgr.load();
        if (!mgr) return 0;

        const int n = ReadPools(mgr, g_buf, kBufMax);

        std::lock_guard<std::mutex> lock(g_setMutex);
        // Drop the stale.
        int w = 0;
        for (int i = 0; i < g_setN; ++i)
            if (nowMs - g_set[i].lastSeenMs <= kKeepMs) g_set[w++] = g_set[i];
        g_setN = w;

        int gimmicks = 0;
        for (int i = 0; i < n; ++i)
        {
            const uintptr_t e = g_buf[i];
            int j = 0;
            for (; j < g_setN; ++j) if (g_set[j].ptr == e) break;
            if (j < g_setN && g_set[j].lastSeenMs == nowMs) continue;   // listed twice this pass
            float pos[3];
            if (!WorldPos(e, pos)) continue;
            if (j == g_setN)
            {
                if (g_setN >= kSetMax) continue;
                g_set[g_setN].ptr = e;
                g_set[g_setN].eid = EidOf(e);
                g_set[g_setN].gimmick = HasGimmick(e);
                ++g_setN;
            }
            g_set[j].x = pos[0]; g_set[j].y = pos[1]; g_set[j].z = pos[2];
            g_set[j].lastSeenMs = nowMs;
        }
        for (int i = 0; i < g_setN; ++i) if (g_set[i].gimmick) ++gimmicks;

        // A line every thirty seconds so the log says what the ray has to work with.
        if (nowMs - g_lastSaidMs > 30000)
        {
            g_lastSaidMs = nowMs;
            GS_LOG("[actors] pools offered %d this pass; set holds %d entities, %d with a gimmick component", n, g_setN, gimmicks);
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
