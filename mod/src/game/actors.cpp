#include "game/actors.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "core/settings.h"
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
    // Master Looter reads to +0x300 because it only ever wants what is near
    // the player. Session fifty-six says that is not enough here: I marked
    // a glint a hundred and nineteen metres due north, the listing swept every
    // entity within sixty-six degrees of the crosshair, and the farthest thing
    // on that bearing was sixty-six metres away. The pools do hold objects at
    // two and three hundred metres in other directions, so the set is not
    // distance-limited, it is incomplete. If the manager keeps more pools
    // further along, this is where they are.
    constexpr uintptr_t kOff_Mgr_PoolsEnd    = 0x1000;
    constexpr uintptr_t kOff_Mgr_PoolsNarrow = 0x300;   // what is known to be readable
    constexpr uintptr_t kOff_Ent_Eid         = 0x60;
    constexpr uintptr_t kOff_Ent_Comps       = 0x68;
    constexpr uintptr_t kOff_Comps_Transform = 0x1A0;
    constexpr uintptr_t kOff_Tf_WorldPos     = 0x29C;
    constexpr uintptr_t kOff_Tf_LocalPos     = 0xB4;
    constexpr uintptr_t kOff_Tf_ParentEid    = 0xC8;
    constexpr uintptr_t kOff_Tf_ParentPos    = 0xEC;
    constexpr uintptr_t kComps_SlotsEnd      = 0x80;
    constexpr uint32_t  kListCapMax          = 0x10000;
    constexpr uint32_t  kKeepMs              = 12000;
    constexpr float     kTeleportMetres      = 300.0f;
    constexpr int       kSetMax              = 4096;
    constexpr int       kBufMax              = 16384;

    std::atomic<uintptr_t> g_vtable{0};
    std::atomic<uintptr_t> g_gimmickVt{0};
    std::atomic<uintptr_t> g_slot{0};      // the global that holds the manager pointer
    int g_gimmicks = 0;
    int g_glints = 0;
    int g_lits = 0;
    int g_pickups = 0;
    std::atomic<uintptr_t> g_mgr{0};
    uint32_t g_checkedAt = 0;
    uintptr_t g_slots[16];
    int g_slotN = 0;
    bool g_slotsScanned = false;
    uint32_t g_lastScanMs = 0;

    std::mutex g_setMutex;
    // On the heap. Entity has members that start non-zero, so as a plain
    // array it is 575 KB of the file itself.
    gs::actors::Entity* const g_set = new gs::actors::Entity[kSetMax];
    float g_lastPlayerX = 0, g_lastPlayerZ = 0;
    bool g_lastPlayerValid = false;
    int g_setN = 0;
    uint32_t g_setGen = 0;   // bumped whenever the set is dropped outside Refresh
    uintptr_t g_buf[kBufMax];              // pool read scratch, tick thread only

    // Where each object sits in the working set, for Refresh alone. A plain
    // table, with no allocation per pass. Twice the set's size, so it is never more than half
    // full and a probe stays short.
    constexpr int kSlotBits = 13;
    constexpr int kSlotCount = 1 << kSlotBits;
    static_assert(kSlotCount >= kSetMax * 2, "the slot table must stay under half full");
    struct Slot { uintptr_t ptr; int at; };
    Slot g_slotOf[kSlotCount];

    int SlotHome(uintptr_t p)
    {
        // Objects are 16-byte aligned, so the low bits carry nothing.
        const uint64_t h = static_cast<uint64_t>(p >> 4) * 0x9E3779B97F4A7C15ull;
        return static_cast<int>(h >> (64 - kSlotBits));
    }

    void SlotsClear() { memset(g_slotOf, 0, sizeof(g_slotOf)); }

    void SlotPut(uintptr_t p, int at)
    {
        for (int i = SlotHome(p);; i = (i + 1) & (kSlotCount - 1))
            if (g_slotOf[i].ptr == 0 || g_slotOf[i].ptr == p) { g_slotOf[i] = {p, at}; return; }
    }

    int SlotFind(uintptr_t p, int missing)
    {
        for (int i = SlotHome(p);; i = (i + 1) & (kSlotCount - 1))
        {
            if (g_slotOf[i].ptr == p) return g_slotOf[i].at;
            if (g_slotOf[i].ptr == 0) return missing;
        }
    }
    uint32_t g_lastSaidMs = 0;

    // Both are defined further down, next to the code they belong with, and
    // both are wanted before that.
    bool PtrLike(uintptr_t p);
    bool StillTheSame(uintptr_t comp, uintptr_t vt);

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
            if (!PtrLike(e)) return false;
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
            // The wide range is a guess at how big the manager is, so it is
            // tried and then given up on rather than failing the whole read.
            uintptr_t poolsEnd = kOff_Mgr_PoolsEnd;
            while (poolsEnd > kOff_Mgr_PoolsNarrow &&
                   !gs::rtti::Readable(reinterpret_cast<const void*>(mgr), poolsEnd))
                poolsEnd -= 0x100;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(mgr), poolsEnd)) return 0;
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

            uintptr_t runs[128];
            uintptr_t runOff[128];
            int runN = 0;
            for (uintptr_t off = kOff_Mgr_PoolsBegin; off + 8 <= poolsEnd && runN < 128; off += 8)
            {
                const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(mgr + off);
                if (arr < 0x10000 || (arr & 7) != 0) continue;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(arr), 8 * 4)) continue;
                const uintptr_t* ents = reinterpret_cast<const uintptr_t*>(arr);
                bool ok = true;
                for (int i = 0; i < 4 && ok; ++i) ok = EntityLike(ents[i]);
                if (ok) { runOff[runN] = off; runs[runN] = arr; ++runN; }
            }
            // Ascending, so a pool that starts inside an earlier run is skipped.
            for (int i = 1; i < runN; ++i)
            {
                const uintptr_t v = runs[i];
                const uintptr_t vo = runOff[i];
                int j = i;
                while (j > 0 && runs[j - 1] > v) { runs[j] = runs[j - 1]; runOff[j] = runOff[j - 1]; --j; }
                runs[j] = v;
                runOff[j] = vo;
            }
            // Sixteen misses in a row ended a run in Master Looter, which only
            // wanted what was near. Sessions twenty-six to thirty-one never
            // had the far glint in the set, so a run now survives five hundred
            // empty slots and the first two passes report what each yields.
            // Session fifty-six spent both reports on the two passes before the
            // world existed and printed nothing, the same way the globals scan
            // once latched on an empty answer. A pass with nothing in it does
            // not count.
            static int statsLeft = 2;
            const bool worthReporting = statsLeft > 0;
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
                    // A pool can end before five hundred empty slots have gone
                    // by, and the walk used to carry on into whatever came next.
                    // On 16 September that was an unmapped allocation boundary,
                    // 000005050C620000, and the fault ended the whole pass, so
                    // every pool after this one went unread until the next.
                    // One question per page is enough to stop at the edge.
                    if ((at & 0xFFF) == 0 && !gs::rtti::Readable(reinterpret_cast<const void*>(at), 8)) break;
                    scanned = i + 1;
                    const uintptr_t e = *reinterpret_cast<const uintptr_t*>(at);
                    if (!EntityLike(e)) { if (++misses >= 512) break; continue; }
                    misses = 0;
                    out[n++] = e;
                }
                coveredTo = at;
                if (worthReporting && n - before > 0)
                    GS_LOG("[actors]   pool %d at manager+0x%03llX -> 0x%p: %u slots walked, %d entities",
                           r, static_cast<unsigned long long>(runOff[r]),
                           reinterpret_cast<void*>(runs[r]), scanned, n - before);
            }
            if (worthReporting && n > 100)
            {
                --statsLeft;
                GS_LOG("[actors] %d pool(s) between manager+0x%03llX and +0x%03llX offered %d entities",
                       runN, static_cast<unsigned long long>(kOff_Mgr_PoolsBegin),
                       static_cast<unsigned long long>(poolsEnd), n);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return n;
    }

    // A value that could be a pointer into the game's heap.
    //
    // The low half has to hold something. Every access violation Master
    // Looter's handler caught inside this file read an address of the shape
    // 0000045A_00000060: a 32-bit value sitting in the high half, zero in the
    // low half, plus the offset of the field being read. A real allocation
    // four gigabytes aligned does not happen, so this costs nothing and stops
    // the probe reading through pool slots that hold a pair of 32-bit values.
    bool PtrLike(uintptr_t p)
    {
        return p >= 0x10000 && (p & 7) == 0 && p <= 0x00007FFFFFFFFFFFull &&
               (p & 0xFFFFFFFFull) != 0;
    }

    bool Sane(const float* v)
    {
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
        return std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[2]) <= 1.0e6f;
    }

    // Where an entity is, in the map's frame.
    //
    // The transform offers the same place in more than one frame and does not
    // say which. There is a cached world position at +0x29C, a local position
    // at +0xB4 in the chunk's frame, a parent id at +0xC8 and the parent's
    // position at +0xEC. Preferring the cached one put a pin twenty metres
    // from the berry patch it named; preferring the local one put every node
    // nine thousand metres away, because the chunk's frame differs from the
    // map's by the chunk origin, which is the player's own world position
    // minus his local one.
    //
    // So all of them are tried and the one that lands near the player wins.
    // Everything in the set was handed over by the manager because it is
    // near him, so an answer that is not is the wrong frame.
    struct Candidate { float p[3]; const char* how; };

    bool WorldPos(uintptr_t e, const gs::player::Pos& pp, float* out, const char** how = nullptr)
    {
        Candidate cand[4];
        int n = 0;
        __try
        {
            const uintptr_t comps = *reinterpret_cast<const uintptr_t*>(e + kOff_Ent_Comps);
            if (!PtrLike(comps)) return false;
            const uintptr_t tf = *reinterpret_cast<const uintptr_t*>(comps + kOff_Comps_Transform);
            if (!PtrLike(tf)) return false;

            float world[3], local[3], pw[3];
            memcpy(world, reinterpret_cast<const void*>(tf + kOff_Tf_WorldPos), 12);
            memcpy(local, reinterpret_cast<const void*>(tf + kOff_Tf_LocalPos), 12);
            memcpy(pw, reinterpret_cast<const void*>(tf + kOff_Tf_ParentPos), 12);
            const uint32_t parent = *reinterpret_cast<const uint32_t*>(tf + kOff_Tf_ParentEid);
            const bool parented = parent != 0xFFFFFFFF && parent != 0;

            if (Sane(world)) { memcpy(cand[n].p, world, 12); cand[n].how = "its cached world position"; ++n; }
            if (Sane(local))
            {
                cand[n].p[0] = local[0] + pp.ox; cand[n].p[1] = local[1] + pp.oy; cand[n].p[2] = local[2] + pp.oz;
                cand[n].how = "its local position and the chunk origin";
                ++n;
                if (parented && Sane(pw))
                {
                    cand[n].p[0] = local[0] + pw[0]; cand[n].p[1] = local[1] + pw[1]; cand[n].p[2] = local[2] + pw[2];
                    cand[n].how = "its local position and its parent's";
                    ++n;
                    cand[n].p[0] = local[0] + pw[0] + pp.ox;
                    cand[n].p[1] = local[1] + pw[1] + pp.oy;
                    cand[n].p[2] = local[2] + pw[2] + pp.oz;
                    cand[n].how = "its local position, its parent's, and the chunk origin";
                    ++n;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        int best = -1;
        float bestD = 1500.0f;   // the manager only hands over what is near
        for (int i = 0; i < n; ++i)
        {
            const float dx = cand[i].p[0] - pp.x, dz = cand[i].p[2] - pp.z;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best < 0) return false;
        memcpy(out, cand[best].p, 12);
        if (how) *how = cand[best].how;
        return true;
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

    // The entity's ClientEffectActorComponent at block slot +0x60.
    uintptr_t EffectComponent(uintptr_t e)
    {
        __try
        {
            const uintptr_t comps = Deref(e + kOff_Ent_Comps);
            if (!comps) return 0;
            const uintptr_t c = Deref(comps + 0x60);
            if (!PtrLike(c)) return 0;
            const uintptr_t vt = Deref(c);
            const char* n = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
            return (n && strstr(n, "ClientEffectActorComponent")) ? c : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // Is the reveal on this entity right now: the detect component's byte,
    // or for a gimmick without one, active custom render values.
    bool ReadLit(uintptr_t detectComp, uintptr_t detectVt, uintptr_t gimmickComp, uintptr_t gimmickVt,
                 bool* out)
    {
        if (!StillTheSame(detectComp, detectVt)) detectComp = 0;
        if (!StillTheSame(gimmickComp, gimmickVt)) gimmickComp = 0;
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

    // A component named by RTTI at a known slot of the block.
    uintptr_t ComponentAt(uintptr_t e, uintptr_t slot, const char* nameFragment)
    {
        __try
        {
            const uintptr_t comps = Deref(e + kOff_Ent_Comps);
            if (!comps) return 0;
            const uintptr_t c = Deref(comps + slot);
            if (!PtrLike(c)) return 0;
            const uintptr_t vt = Deref(c);
            const char* n = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
            return (n && strstr(n, nameFragment)) ? c : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // A string the engine keeps as a pointer to an object whose first field
    // is the characters. Master Looter's ReadEngineString.
    bool EngineString(uintptr_t slot, char* out, size_t n)
    {
        __try
        {
            const uintptr_t obj = Deref(slot);
            if (!PtrLike(obj)) return false;
            const uintptr_t cstr = Deref(obj);
            if (!cstr || !gs::rtti::Readable(reinterpret_cast<const void*>(cstr), 1)) return false;
            size_t i = 0;
            for (; i + 1 < n; ++i)
            {
                const char c = *reinterpret_cast<const volatile char*>(cstr + i);
                if (c == 0) break;
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return false;
                out[i] = c;
            }
            out[i] = 0;
            return i > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The last path element without its extension: the whole path is too
    // long for a log line and the leaf is the part that names the thing.
    void Leaf(const char* path, char* out, size_t n)
    {
        const char* slash = strrchr(path, '/');
        const char* start = slash ? slash + 1 : path;
        strncpy_s(out, n, start, _TRUNCATE);
        char* dot = strstr(out, ".prefab");
        if (dot) *dot = 0;
    }

    // Is this node one the player would want a pin on.
    //
    // Two sessions named what stands around the player. At the first glint:
    // gimmick_item_firewood_0001, gimmick_item_basic_visione_chip,
    // gimmick_socket_collection_sophora_01, trees with scenecollection in
    // the path. At the second, a puzzle glint with nothing to take yet:
    // gimmick_standstone_01_challenge, beside gimmick_operator_gimmick
    // complete_00, gimmick_func_puzzle_trigger_box and pointcontrol nodes,
    // which are the puzzle's invisible machinery and must never be marked.
    //
    // So the name decides. The rejects win, because a trigger box sitting a
    // metre from the standing stone would otherwise take the pin.
    // The reject list again, against a node's stored leaf name rather than
    // its full path, so a candidate can be judged after the fact.
    bool Machinery(const char* name)
    {
        static const char* const reject[] = {
            "func_", "operator_", "trigger", "pointcontrol", "camera", "fit_height",
            "volume", "sector", "spawn", "collision", "phase00", "_once"};
        if (!name || !name[0]) return true;   // nameless is not worth a pin
        for (const char* r : reject)
            if (strstr(name, r)) return true;
        return false;
    }

    bool WorthMarking(const char* path)
    {
        static const char* const reject[] = {
            "func_", "operator_", "trigger", "pointcontrol", "camera", "fit_height",
            "volume", "sector", "spawn", "collision", "phase00", "_once"};
        for (const char* r : reject)
            if (strstr(path, r)) return false;

        // The rest is the player's list, from the ini.
        return gs::Settings::Marked(path);
    }

    // What this gimmick is, and whether the player can take something from
    // it. Master Looter identifies a node by its prefab path, and its log
    // was still naming nodes correctly on 2850 while this mod's item data
    // test found nothing near the player: "/object/cd_gimmick/00_common/
    // item/gimmick_item_trade_salt_02.prefab" is an item, ".../gather/..."
    // or a path with "gather" in it is a gather node. The item and gather
    // data pointers are read too, but a node is a pickup if either the path
    // or a pointer says so, since the pointers are only filled once the node
    // is armed.
    bool Pickup(uintptr_t comp, bool* locked, char* name, size_t nameBytes)
    {
        __try
        {
            char path[256];
            const uintptr_t pf = Deref(comp + gs::sig::kOff_Gimmick_Prefab);
            bool havePath = pf && EngineString(pf + gs::sig::kOff_Prefab_Path, path, sizeof(path));
            if (!havePath)
            {
                const uintptr_t alt = Deref(comp + gs::sig::kOff_Gimmick_PrefabAlt);
                havePath = alt && EngineString(alt + gs::sig::kOff_PrefabAlt_Path, path, sizeof(path));
            }

            const uintptr_t item = *reinterpret_cast<const uintptr_t*>(comp + gs::sig::kOff_Gimmick_ItemData);
            const uintptr_t gather = *reinterpret_cast<const uintptr_t*>(comp + gs::sig::kOff_Gimmick_GatherData);
            const bool byData = PtrLike(item) || PtrLike(gather);

            bool byPath = false;
            if (havePath)
            {
                byPath = WorthMarking(path);
                Leaf(path, name, nameBytes);
            }
            else if (!EngineString(comp + gs::sig::kOff_Gimmick_NodeName, name, nameBytes))
                name[0] = 0;

            // A node whose path says it is machinery is machinery, whatever
            // its data pointers hold.
            if (havePath && !byPath) return false;
            if (!byData && !byPath) return false;
            *locked = *reinterpret_cast<const uint8_t*>(comp + gs::sig::kOff_Gimmick_Locked) != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The prefab leaf of any gimmick, pickup or not, for the log.
    bool GimmickName(uintptr_t comp, char* out, size_t n)
    {
        __try
        {
            char path[256];
            const uintptr_t pf = Deref(comp + gs::sig::kOff_Gimmick_Prefab);
            if (pf && EngineString(pf + gs::sig::kOff_Prefab_Path, path, sizeof(path))) { Leaf(path, out, n); return true; }
            const uintptr_t alt = Deref(comp + gs::sig::kOff_Gimmick_PrefabAlt);
            if (alt && EngineString(alt + gs::sig::kOff_PrefabAlt_Path, path, sizeof(path))) { Leaf(path, out, n); return true; }
            return EngineString(comp + gs::sig::kOff_Gimmick_NodeName, out, n);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The detect mode target byte on a gimmick component.
    // Still the object this component was? A freed block keeps its bytes
    // until the game hands the memory to something else, and then the first
    // qword is that something else's vtable. One compare says which it is.
    bool StillTheSame(uintptr_t comp, uintptr_t vt)
    {
        __try
        {
            return comp != 0 && vt != 0 && *reinterpret_cast<const uintptr_t*>(comp) == vt;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool GlintByte(uintptr_t comp, uintptr_t vt, bool* out)
    {
        if (!StillTheSame(comp, vt)) return false;
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

    void Forget(const char* why)
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        if (!g_setN) return;
        GS_LOG("[actors] %s, so the %d entities held from before it are dropped", why ? why : "the world changed",
               g_setN);
        g_setN = 0;
        ++g_setGen;
        g_lastPlayerValid = false;
    }

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

        // The first look can come before the game has built a manager, and
        // session forty-nine did exactly that: zero globals at startup, the
        // answer latched, and the entity set stayed empty for the whole
        // session. An empty answer is not an answer, so it is asked again.
        if (!g_slotsScanned || (g_slotN == 0 && nowMs - g_lastScanMs > 3000))
        {
            g_slotsScanned = true;
            g_lastScanMs = nowMs;
            const int before = g_slotN;
            g_slotN = FindGlobals(vt, g_slots, 16);
            if (g_slotN != before || g_slotN > 0)
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

    int Offered(uintptr_t* out, int n)
    {
        const uintptr_t mgr = g_mgr.load();
        if (!mgr || !out || n <= 0) return 0;
        return ReadPools(mgr, out, n);
    }

    uint32_t Refresh(uint32_t nowMs)
    {
        const uintptr_t mgr = g_mgr.load();
        if (!mgr) return 0;

        const int n = ReadPools(mgr, g_buf, kBufMax);

        // The set is worked on as a copy and put back in one short step.
        //
        // This held g_setMutex for the whole pass, and a pass is every pool
        // entry read, positioned and looked up by class: on 19 September
        // hawkeye69's world offered 5920 entries against 1034 held, with a
        // nested search for each. The automatic marker reads the set under
        // that lock from the game's own UI thread every quarter second while
        // the flash is up, so a frame that arrived mid-pass waited for all of
        // it, and he saw the flash stutter.
        // On the heap, once, for the same reason as g_set.
        static gs::actors::Entity* const s_work = new gs::actors::Entity[kSetMax];
        int workN = 0;
        uint32_t gen = 0;
        {
            std::lock_guard<std::mutex> lock(g_setMutex);
            workN = g_setN;
            for (int i = 0; i < workN; ++i) s_work[i] = g_set[i];
            gen = g_setGen;
        }

        // A world change frees every object in the set at once, and the set
        // would otherwise go on reading them for twelve seconds. That is where
        // the access violations in this file came from, all of them within
        // half a minute of a teleport, and where the nonsense came from that
        // did not fault: on 16 September the set held an object 5,240 metres
        // away with its glint byte reading as set, which is a freed block that
        // something else had moved into.
        //
        // A jump nothing can walk or fly is the signal. Three hundred metres
        // between two passes half a second apart is six hundred metres a
        // second, well past a wyvern, and every teleport in that morning's
        // logs moved between nine hundred and sixteen hundred.
        //
        // One read of the player for the whole pass. There used to be two, one
        // here and one for the positions below, and the game can answer them
        // differently half a second apart.
        const gs::player::Pos pp = gs::player::Read();

        // While a world loads the player reads as a placeholder near the
        // absolute origin, and in some places it flips between that and his
        // real position every pass or two. The first build with the jump check
        // took each flip for a teleport: on 16 September it dropped the set
        // twenty-seven times in nineteen minutes. Five of those were real fast
        // travels of one to four kilometres. The other twenty-two measured 10
        // to 13 km, which is exactly how far that part of the map sits from
        // (0, 0), and in between the flips it filled the set with objects
        // placed at (0.0, 0.0, 0.0).
        // The pinning side has refused the placeholder since session
        // fifty-three with the same rule, a player within a hundred of the
        // origin, so the set now sits those passes out altogether and keeps
        // what it had.
        if (pp.valid && std::fabs(pp.x) + std::fabs(pp.z) < 100.0f)
        {
            static int saidLeft = 3;
            if (saidLeft > 0)
            {
                --saidLeft;
                GS_LOG("[actors] the player reads (%.1f, %.1f, %.1f), the placeholder a loading world "
                       "uses, so this pass leaves the set alone", pp.x, pp.y, pp.z);
            }
            return static_cast<uint32_t>(n);
        }

        if (pp.valid && g_lastPlayerValid)
        {
            const float dx = pp.x - g_lastPlayerX, dz = pp.z - g_lastPlayerZ;
            const float moved = std::sqrt(dx * dx + dz * dz);
            if (moved > kTeleportMetres && workN > 0)
            {
                char why[64];
                snprintf(why, sizeof(why), "the player moved %.0f metres in one pass", moved);
                GS_LOG("[actors] %s, so the %d entities from where he was are dropped rather "
                       "than read for another twelve seconds", why, workN);
                workN = 0;
            }
        }
        g_lastPlayerValid = pp.valid;
        if (pp.valid) { g_lastPlayerX = pp.x; g_lastPlayerZ = pp.z; }

        // Drop the stale.
        int w = 0;
        for (int i = 0; i < workN; ++i)
            if (nowMs - s_work[i].lastSeenMs <= kKeepMs) s_work[w++] = s_work[i];
        workN = w;

        SlotsClear();
        for (int i = 0; i < workN; ++i) SlotPut(s_work[i].ptr, i);

        int gimmicks = 0, dupes = 0, noPos = 0;
        for (int i = 0; i < n; ++i)
        {
            const uintptr_t e = g_buf[i];
            int j = e ? SlotFind(e, workN) : workN;
            if (j < workN && s_work[j].lastSeenMs == nowMs) { ++dupes; continue; }   // listed twice this pass
            float pos[3];
            const char* how = "";
            if (!pp.valid || !WorldPos(e, pp, pos, &how))
            {
                ++noPos;
                if (g_failLogsLeft > 0 && pp.valid && (i % 7) == 3)
                {
                    --g_failLogsLeft;
                    DescribeFailure(e, pp);
                }
                continue;
            }
            if (j == workN)
            {
                if (workN >= kSetMax) continue;
                Entity& ne = s_work[workN];
                ne = Entity{};
                ne.ptr = e;
                ne.eid = EidOf(e);
                ne.gimmickComp = GimmickComponent(e);
                ne.gimmick = ne.gimmickComp != 0;
                ne.gimmickVt = ne.gimmickComp ? Deref(ne.gimmickComp) : 0;
                ne.detectComp = DetectComponent(e);
                ne.detectVt = ne.detectComp ? Deref(ne.detectComp) : 0;
                ne.effectComp = EffectComponent(e);
                ne.knowledge = ComponentAt(e, gs::sig::kOff_Comps_Knowledge, "Knowledge") != 0;
                if (ne.gimmickComp)
                {
                    bool locked = false;
                    ne.pickup = Pickup(ne.gimmickComp, &locked, ne.name, sizeof(ne.name));
                    ne.locked = locked;
                    if (!ne.name[0]) GimmickName(ne.gimmickComp, ne.name, sizeof(ne.name));
                }
                SlotPut(e, workN);
                ++workN;
            }
            Entity& en = s_work[j];
            en.x = pos[0]; en.y = pos[1]; en.z = pos[2];
            en.how = how;
            en.lastSeenMs = nowMs;
            // The glint byte, every pass: the event that sets it can fire any time.
            bool g = false;
            if (en.gimmickComp && GlintByte(en.gimmickComp, en.gimmickVt, &g))
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
            else if (en.gimmickComp && !StillTheSame(en.gimmickComp, en.gimmickVt))
            {
                // The block is somebody else's now. The object does not glint
                // because nothing here knows whether it does, and leaving the
                // last answer standing is how a freed node keeps a pin.
                en.gimmickComp = 0;
                en.gimmickVt = 0;
                en.gimmick = false;
                en.glint = false;
            }
            if (en.detectComp && !StillTheSame(en.detectComp, en.detectVt))
            {
                en.detectComp = 0;
                en.detectVt = 0;
                en.lit = false;
            }
            // The reveal, every pass, and its first flips in the log.
            bool lit = false;
            if (ReadLit(en.detectComp, en.detectVt, en.gimmickComp, en.gimmickVt, &lit))
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
        int glints = 0, lits = 0, pickups = 0;
        for (int i = 0; i < workN; ++i)
        {
            if (s_work[i].gimmick) ++gimmicks;
            if (s_work[i].glint) ++glints;
            if (s_work[i].lit) ++lits;
            if (s_work[i].pickup) ++pickups;
        }
        {
            std::lock_guard<std::mutex> lock(g_setMutex);
            // Forget ran while this pass was reading, so what it read is the
            // old world's. Dropped, and the next pass starts from nothing.
            if (gen != g_setGen) return static_cast<uint32_t>(n);
            for (int i = 0; i < workN; ++i) g_set[i] = s_work[i];
            g_setN = workN;
            g_gimmicks = gimmicks;
            g_glints = glints;
            g_lits = lits;
            g_pickups = pickups;
        }

        // A line every thirty seconds so the log says what the ray has to work with.
        if (nowMs - g_lastSaidMs > 30000)
        {
            g_lastSaidMs = nowMs;
            GS_LOG("[actors] pools offered %d this pass (%d listed twice, %d without a position); set holds %d entities, %d gimmicks, %d of them pickups, %d with the glint byte set, %d lit",
                   n, dupes, noPos, workN, gimmicks, pickups, glints, lits);
            // The nearest pickups, which is what the player can actually see.
            int order[6];
            float dist[6];
            int named = 0;
            for (int i = 0; i < workN; ++i)
            {
                if (!s_work[i].pickup) continue;
                const float dx = s_work[i].x - pp.x, dz = s_work[i].z - pp.z;
                const float d = std::sqrt(dx * dx + dz * dz);
                if (named == 6 && d >= dist[5]) continue;
                int pos2 = named < 6 ? named : 5;
                while (pos2 > 0 && dist[pos2 - 1] > d) { dist[pos2] = dist[pos2 - 1]; order[pos2] = order[pos2 - 1]; --pos2; }
                dist[pos2] = d; order[pos2] = i;
                if (named < 6) ++named;
            }
            for (int k = 0; k < named; ++k)
            {
                const Entity& en = s_work[order[k]];
                GS_LOG("[actors]   pickup eid %08X \"%s\"%s%s at (%.1f, %.1f, %.1f), %.0f away, from %s", en.eid,
                       en.name[0] ? en.name : "?", en.knowledge ? ", knowledge" : "", en.locked ? ", locked" : "",
                       en.x, en.y, en.z, dist[k], en.how ? en.how : "?");
            }
            int shown = 0;
            for (int i = 0; i < workN && shown < 3; ++i)
                if (s_work[i].glint)
                {
                    ++shown;
                    const float dx = s_work[i].x - pp.x, dz = s_work[i].z - pp.z;
                    GS_LOG("[actors]   byte set on eid %08X at (%.1f, %.1f, %.1f), %.0f away", s_work[i].eid, s_work[i].x, s_work[i].y, s_work[i].z, std::sqrt(dx * dx + dz * dz));
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

    int PickupCount()
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        return g_pickups;
    }

    int MarkedNear(float px, float pz, float radius, Entity* out, int n)
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        int found = 0;
        for (int i = 0; i < g_setN; ++i)
        {
            if (!g_set[i].pickup && !(g_set[i].gimmick && g_set[i].knowledge)) continue;
            const float dx = g_set[i].x - px, dz = g_set[i].z - pz;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d > radius) continue;
            int pos = found;
            while (pos > 0)
            {
                const float ax = out[pos - 1].x - px, az = out[pos - 1].z - pz;
                if (std::sqrt(ax * ax + az * az) <= d) break;
                if (pos < n) out[pos] = out[pos - 1];
                --pos;
            }
            if (pos < n) out[pos] = g_set[i];
            if (found < n) ++found;
        }
        return found;
    }

    void LogEntities(float px, float pz, float ox, float oz, float ux, float uz)
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        int shown = 0;
        GS_LOG("[set] every entity the pools hold, by how far off the crosshair it sits:");
        // Smallest bearing error first, so the ones the crosshair could
        // plausibly be on come first and a long tail can be read or ignored.
        for (int rank = 0; rank < 120; ++rank)
        {
            int best = -1;
            float bestAngle = 0;
            for (int i = 0; i < g_setN; ++i)
            {
                if (g_set[i].shown) continue;
                const float dx = g_set[i].x - ox, dz = g_set[i].z - oz;
                const float flat = std::sqrt(dx * dx + dz * dz);
                if (flat < 0.5f) continue;
                const float dot = (dx * ux + dz * uz) / flat;
                const float cross = (dx * uz - dz * ux) / flat;
                const float angle = std::fabs(std::atan2(cross, dot));
                if (best < 0 || angle < bestAngle) { best = i; bestAngle = angle; }
            }
            if (best < 0) break;
            Entity& e = g_set[best];
            e.shown = true;
            const float dx = e.x - px, dz = e.z - pz;
            const char* cls = "?";
            const uintptr_t vt = Deref(e.ptr);
            if (vt)
            {
                const char* n = gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt));
                if (n) cls = (n[0] == '.') ? n + 4 : n;
            }
            const char* what = !e.gimmick ? "not a gimmick"
                               : (e.pickup ? "candidate, on the name list"
                                           : (Machinery(e.name) ? "machinery" : "candidate"));
            GS_LOG("[set]   %6.1f deg  %6.1f m  %-42s %-34s eid %08X  %s%s", bestAngle * 57.2958f,
                   std::sqrt(dx * dx + dz * dz), e.name[0] ? e.name : "(no name)", cls, e.eid,
                   what, e.knowledge ? ", knowledge" : "");
            ++shown;
        }
        for (int i = 0; i < g_setN; ++i) g_set[i].shown = false;
        GS_LOG("[set] %d entity(s) listed of %d in the set", shown, g_setN);
    }

    int GlintOnBearing(float px, float pz, float ox, float oz, float ux, float uz,
                       Entity* out, float* angles, int n)
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        int found = 0;
        for (int i = 0; i < g_setN; ++i)
        {
            if (!g_set[i].glint) continue;
            const float dx = g_set[i].x - ox, dz = g_set[i].z - oz;
            const float flat = std::sqrt(dx * dx + dz * dz);
            if (flat < 0.5f) continue;
            const float dot = (dx * ux + dz * uz) / flat;
            const float cross = (dx * uz - dz * ux) / flat;
            const float angle = std::fabs(std::atan2(cross, dot));
            int pos = found;
            while (pos > 0 && angles[pos - 1] > angle)
            {
                if (pos < n) { out[pos] = out[pos - 1]; angles[pos] = angles[pos - 1]; }
                --pos;
            }
            if (pos < n) { out[pos] = g_set[i]; angles[pos] = angle; }
            if (found < n) ++found;
        }
        (void)px; (void)pz;
        return found;
    }

    bool InSet(uintptr_t ptr)
    {
        if (!ptr) return false;
        std::lock_guard<std::mutex> lock(g_setMutex);
        for (int i = 0; i < g_setN; ++i)
            if (g_set[i].ptr == ptr) return true;
        return false;
    }

    float MarkedReach(float px, float pz)
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        float far_ = 0.0f;
        for (int i = 0; i < g_setN; ++i)
        {
            if (!g_set[i].pickup && !(g_set[i].gimmick && g_set[i].knowledge)) continue;
            const float dx = g_set[i].x - px, dz = g_set[i].z - pz;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d > far_) far_ = d;
        }
        return far_;
    }

    int MarkedOnBearing(float px, float pz, float ox, float oz, float ux, float uz,
                        float radius, float maxAngle, float minFromPlayer,
                        Entity* out, float* angles, int n, int* marked)
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        int found = 0;
        int inRadius = 0;
        for (int i = 0; i < g_setN; ++i)
        {
            // Every gimmick that is not machinery, not only the ones whose
            // name is on the ini's list. Session forty-seven measured the
            // miss: my own marker on the glint sat a hundred and eighteen
            // metres north of me and the mod pinned a berry bush twenty-one
            // metres north, because berry bushes were the only things the
            // name list let through. The angle decides, and the angle is
            // harder on near things than far ones, so a wider field is safe:
            // a bush twenty metres out has to sit within a third of a metre
            // of the line to beat a glint a hundred metres out sitting within
            // a metre and a half of it.
            if (!g_set[i].gimmick) continue;
            if (!g_set[i].pickup && Machinery(g_set[i].name)) continue;
            const float px2 = g_set[i].x - px, pz2 = g_set[i].z - pz;
            const float fromPlayer = std::sqrt(px2 * px2 + pz2 * pz2);
            if (fromPlayer > radius) continue;
            ++inRadius;
            if (fromPlayer < minFromPlayer) continue;
            const float dx = g_set[i].x - ox, dz = g_set[i].z - oz;
            const float flat = std::sqrt(dx * dx + dz * dz);
            if (flat < 0.5f) continue;
            const float dot = (dx * ux + dz * uz) / flat;
            const float cross = (dx * uz - dz * ux) / flat;
            const float angle = std::fabs(std::atan2(cross, dot));
            if (angle >= maxAngle) continue;
            int pos = found;
            while (pos > 0 && angles[pos - 1] > angle)
            {
                if (pos < n) { out[pos] = out[pos - 1]; angles[pos] = angles[pos - 1]; }
                --pos;
            }
            if (pos < n) { out[pos] = g_set[i]; angles[pos] = angle; }
            if (found < n) ++found;
        }
        if (marked) *marked = inRadius;
        return found;
    }

    int LitNear(float px, float py, float pz, Entity* out, int n)
    {
        std::lock_guard<std::mutex> lock(g_setMutex);
        int found = 0;
        for (int i = 0; i < g_setN; ++i)
        {
            if (!g_set[i].lit) continue;
            const float dx = g_set[i].x - px, dy = g_set[i].y - py, dz = g_set[i].z - pz;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            int pos = found;
            while (pos > 0)
            {
                const float ax = out[pos - 1].x - px, ay = out[pos - 1].y - py, az = out[pos - 1].z - pz;
                if (std::sqrt(ax * ax + ay * ay + az * az) <= d) break;
                if (pos < n) out[pos] = out[pos - 1];
                --pos;
            }
            if (pos < n) out[pos] = g_set[i];
            if (found < n) ++found;
        }
        return found;
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

    uintptr_t PlayerEntity(uintptr_t* specialComponent)
    {
        const uintptr_t mgr = g_mgr.load();
        if (!mgr) return 0;
        static uintptr_t buf[kBufMax];
        const int n = ReadPools(mgr, buf, kBufMax);
        for (int i = 0; i < n; ++i)
        {
            const uint32_t eid = EidOf(buf[i]);
            if ((eid >> 24) != 0xA0) continue;
            // Every character carries a special mode component; the player's
            // entity is the one the manager files under a player id.
            const uintptr_t sp = ComponentAt(buf[i], 0x10, "ClientSpecialModeActorComponent");
            uintptr_t found = sp;
            if (!found)
                for (uintptr_t off = 0; off < kComps_SlotsEnd && !found; off += 8)
                    found = ComponentAt(buf[i], off, "ClientSpecialModeActorComponent");
            if (!found) continue;
            if (specialComponent) *specialComponent = found;
            return buf[i];
        }
        return 0;
    }

    uintptr_t ByEid(uint32_t eid)
    {
        if (!eid || eid == 0xFFFFFFFF) return 0;
        std::lock_guard<std::mutex> lock(g_setMutex);
        for (int i = 0; i < g_setN; ++i) if (g_set[i].eid == eid) return g_set[i].ptr;
        return 0;
    }
}
