#include "game/actors.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "game/aim.h"
#include "game/player.h"
#include "game/rtti.h"
#include "game/signatures.h"
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
    std::atomic<uintptr_t> g_gimmickVt{0};
    std::atomic<uintptr_t> g_slot{0};      // the global that holds the manager pointer
    int g_gimmicks = 0;
    int g_glints = 0;
    int g_lits = 0;
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
            // Sixteen misses in a row ended a run in Master Looter, which only
            // wanted what was near. Sessions twenty-six to thirty-one never
            // had the far glint in the set, so a run now survives five hundred
            // empty slots and the first two passes report what each yields.
            static int statsLeft = 2;
            uintptr_t coveredTo = 0;
            for (int r = 0; r < runN && n < cap; ++r)
            {
                if (runs[r] < coveredTo) continue;
                uintptr_t at = runs[r];
                int misses = 0;
                uint32_t scanned = 0;
                const int before = n;
                for (uint32_t i = 0; i < 20000 && n < cap; ++i, at += 8)
                {
                    scanned = i + 1;
                    const uintptr_t e = *reinterpret_cast<const uintptr_t*>(at);
                    if (!EntityLike(e)) { if (++misses >= 512) break; continue; }
                    misses = 0;
                    out[n++] = e;
                }
                coveredTo = at;
                if (statsLeft > 0)
                    GS_LOG("[actors]   pool %d at 0x%p: %u slots walked, %d entities", r, reinterpret_cast<void*>(runs[r]), scanned, n - before);
            }
            if (statsLeft > 0) --statsLeft;
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
            // Not placed yet: session twenty-six had entities at the origin.
            if (out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f) return false;
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

    // The entity's ClientGimmickActorComponent, or 0. It sits at slot +0x30
    // of the block; with the vtable known that is one compare, otherwise the
    // slots are named through RTTI. Read once when the entity joins the set.
    uintptr_t GimmickComponent(uintptr_t e)
    {
        __try
        {
            const uintptr_t comps = Deref(e + kOff_Ent_Comps);
            if (!comps || !gs::rtti::Readable(reinterpret_cast<const void*>(comps), kComps_SlotsEnd)) return 0;
            const uintptr_t known = g_gimmickVt.load();
            if (known)
            {
                const uintptr_t c = *reinterpret_cast<const uintptr_t*>(comps + gs::sig::kOff_Comps_Gimmick);
                if (PtrLike(c) && Deref(c) == known) return c;
            }
            for (uintptr_t off = 0; off < kComps_SlotsEnd; off += 8)
            {
                const uintptr_t c = *reinterpret_cast<const uintptr_t*>(comps + off);
                if (!PtrLike(c)) continue;
                const uintptr_t vt = Deref(c);
                if (known && vt == known) return c;
                const char* n = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
                if (n && strstr(n, "ClientGimmickActorComponent")) return c;
            }
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // The entity's own ClientDetectActorComponent at block slot +0x50, named
    // through RTTI once, when the entity joins the set.
    uintptr_t DetectComponent(uintptr_t e)
    {
        __try
        {
            const uintptr_t comps = Deref(e + kOff_Ent_Comps);
            if (!comps) return 0;
            const uintptr_t c = Deref(comps + gs::sig::kOff_Comps_Detect);
            if (!PtrLike(c)) return 0;
            const uintptr_t vt = Deref(c);
            const char* n = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
            return (n && strstr(n, "ClientDetectActorComponent")) ? c : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // Is the reveal on this entity right now: the detect component's byte,
    // or for a gimmick without one, active custom render values.
    bool ReadLit(uintptr_t detectComp, uintptr_t gimmickComp, bool* out)
    {
        __try
        {
            if (detectComp)
            {
                *out = *reinterpret_cast<const uint8_t*>(detectComp + gs::sig::kOff_Detect_Lit) != 0;
                return true;
            }
            if (gimmickComp)
            {
                const uintptr_t sub = *reinterpret_cast<const uintptr_t*>(gimmickComp + gs::sig::kOff_Gimmick_Sub);
                if (!PtrLike(sub)) return false;
                *out = *reinterpret_cast<const uint32_t*>(sub + gs::sig::kOff_GimmickSub_Active) != 0;
                return true;
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The detect mode target byte on a gimmick component.
    bool GlintByte(uintptr_t comp, bool* out)
    {
        __try
        {
            *out = *reinterpret_cast<const uint8_t*>(comp + gs::sig::kOff_Gimmick_DetectTgt) != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Why an entity has no position. Session twenty-five kept 229 of 946
    // offered and the glint the player looked at was not among them, so
    // the first few entities whose transform walk fails are described:
    // class, every component in the block, and for any component named
    // Transform, the offsets inside it holding a float3 near the player.
    int g_failLogsLeft = 8;

    const char* Short(const char* n)
    {
        if (!n) return "?";
        return n[0] == '.' ? n + 4 : n;
    }

    void FindPositionIn(uintptr_t comp, const gs::player::Pos& pp)
    {
        __try
        {
            size_t bytes = 0x400;
            while (bytes >= 0x40 && !gs::rtti::Readable(reinterpret_cast<const void*>(comp), bytes)) bytes /= 2;
            if (bytes < 0x40) return;
            const auto* b = reinterpret_cast<const uint8_t*>(comp);
            int found = 0;
            for (size_t off = 0; off + 12 <= bytes && found < 6; off += 4)
            {
                float v[3];
                memcpy(v, b + off, 12);
                if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
                const float dw = std::fabs(v[0] - pp.x) + std::fabs(v[1] - pp.y) + std::fabs(v[2] - pp.z);
                const float dl = std::fabs(v[0] - pp.lx) + std::fabs(v[1] - pp.ly) + std::fabs(v[2] - pp.lz);
                if (dw < 150.0f || dl < 150.0f)
                {
                    ++found;
                    GS_LOG("[actors]       +0x%03llX (%.1f, %.1f, %.1f) is %s space, %.0f from the player",
                           static_cast<unsigned long long>(off), v[0], v[1], v[2], dw < dl ? "world" : "local",
                           dw < dl ? dw : dl);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void DescribeFailure(uintptr_t e, const gs::player::Pos& pp)
    {
        const uintptr_t vt = Deref(e);
        const char* cn = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
        const uintptr_t comps = Deref(e + kOff_Ent_Comps);
        GS_LOG("[actors] no position for eid %08X %s, block 0x%p, +0x1A0 -> 0x%p",
               EidOf(e), Short(cn), reinterpret_cast<void*>(comps), reinterpret_cast<void*>(comps ? Deref(comps + kOff_Comps_Transform) : 0));
        if (!comps) return;
        for (uintptr_t off = 0; off < 0x200; off += 8)
        {
            const uintptr_t c = Deref(comps + off);
            if (!PtrLike(c)) continue;
            const uintptr_t cvt = Deref(c);
            const char* n = cvt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(cvt)) : nullptr;
            if (!n) continue;
            GS_LOG("[actors]     block+0x%03llX 0x%p %s", static_cast<unsigned long long>(off), reinterpret_cast<void*>(c), Short(n));
            if (strstr(n, "Transform")) FindPositionIn(c, pp);
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
    void SetGimmickVtable(uintptr_t vtable) { g_gimmickVt.store(vtable); }

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

        int gimmicks = 0, dupes = 0, noPos = 0;
        const gs::player::Pos pp = gs::player::Read();
        for (int i = 0; i < n; ++i)
        {
            const uintptr_t e = g_buf[i];
            int j = 0;
            for (; j < g_setN; ++j) if (g_set[j].ptr == e) break;
            if (j < g_setN && g_set[j].lastSeenMs == nowMs) { ++dupes; continue; }   // listed twice this pass
            float pos[3];
            if (!WorldPos(e, pos))
            {
                ++noPos;
                if (g_failLogsLeft > 0 && pp.valid && (i % 7) == 3)
                {
                    --g_failLogsLeft;
                    DescribeFailure(e, pp);
                }
                continue;
            }
            if (j == g_setN)
            {
                if (g_setN >= kSetMax) continue;
                Entity& ne = g_set[g_setN];
                ne = Entity{};
                ne.ptr = e;
                ne.eid = EidOf(e);
                ne.gimmickComp = GimmickComponent(e);
                ne.gimmick = ne.gimmickComp != 0;
                ne.detectComp = DetectComponent(e);
                ++g_setN;
            }
            Entity& en = g_set[j];
            en.x = pos[0]; en.y = pos[1]; en.z = pos[2];
            en.lastSeenMs = nowMs;
            // The glint byte, every pass: the event that sets it can fire any time.
            bool g = false;
            if (en.gimmickComp && GlintByte(en.gimmickComp, &g))
            {
                static int flipsLeft = 20;
                if (g != en.glint && flipsLeft > 0)
                {
                    --flipsLeft;
                    const uintptr_t vt = Deref(en.ptr);
                    const char* cn = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
                    GS_LOG("[actors] glint byte %s on eid %08X %s at (%.1f, %.1f, %.1f), flash %s", g ? "set" : "cleared", en.eid,
                           cn ? (cn[0] == '.' ? cn + 4 : cn) : "?", en.x, en.y, en.z, gs::aim::FlashActive() ? "on" : "off");
                }
                en.glint = g;
            }
            // The reveal, every pass, and its first flips in the log.
            bool lit = false;
            if (ReadLit(en.detectComp, en.gimmickComp, &lit))
            {
                static int litFlipsLeft = 30;
                if (lit != en.lit && litFlipsLeft > 0)
                {
                    --litFlipsLeft;
                    const uintptr_t vt = Deref(en.ptr);
                    const char* cn = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
                    const float dx = en.x - pp.x, dz = en.z - pp.z;
                    GS_LOG("[actors] reveal %s on eid %08X %s%s at (%.1f, %.1f, %.1f), %.0f away, flash %s, via %s",
                           lit ? "ON" : "off", en.eid, en.gimmick ? "gimmick " : "", cn ? (cn[0] == '.' ? cn + 4 : cn) : "?",
                           en.x, en.y, en.z, std::sqrt(dx * dx + dz * dz), gs::aim::FlashActive() ? "on" : "off",
                           en.detectComp ? "the detect component" : "the gimmick's render values");
                }
                en.lit = lit;
            }
        }
        int glints = 0, lits = 0;
        for (int i = 0; i < g_setN; ++i)
        {
            if (g_set[i].gimmick) ++gimmicks;
            if (g_set[i].glint) ++glints;
            if (g_set[i].lit) ++lits;
        }
        g_gimmicks = gimmicks;
        g_glints = glints;
        g_lits = lits;

        // A line every thirty seconds so the log says what the ray has to work with.
        if (nowMs - g_lastSaidMs > 30000)
        {
            g_lastSaidMs = nowMs;
            GS_LOG("[actors] pools offered %d this pass (%d listed twice, %d without a position); set holds %d entities, %d with a gimmick component, %d with the byte, %d lit by the flash",
                   n, dupes, noPos, g_setN, gimmicks, glints, lits);
            int shown = 0;
            for (int i = 0; i < g_setN && shown < 3; ++i)
                if (g_set[i].glint)
                {
                    ++shown;
                    const float dx = g_set[i].x - pp.x, dz = g_set[i].z - pp.z;
                    GS_LOG("[actors]   byte set on eid %08X at (%.1f, %.1f, %.1f), %.0f away", g_set[i].eid, g_set[i].x, g_set[i].y, g_set[i].z, std::sqrt(dx * dx + dz * dz));
                }
        }
        return static_cast<uint32_t>(n);
    }

    int GimmickCount()
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        return g_gimmicks;
    }

    int GlintCount()
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        return g_glints;
    }

    int LitCount()
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        return g_lits;
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
