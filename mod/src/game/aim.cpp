#include "game/aim.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/actors.h"
#include "game/rtti.h"
#include "game/signatures.h"

namespace
{
    constexpr uintptr_t kOff_Ent_Comps       = 0x68;
    constexpr uintptr_t kOff_Comps_Transform = 0x1A0;
    constexpr uintptr_t kOff_Tf_Pos          = 0xB4;
    constexpr uintptr_t kOff_Tf_ParentEid    = 0xC8;
    constexpr uintptr_t kOff_Tf_ParentPos    = 0xEC;
    constexpr uintptr_t kOff_Special_Active  = 0x40;   // player id while the flash is on
    constexpr uintptr_t kOff_Special_Mode    = 0x30;   // the mode id at the head of that record

    // The game's special modes, in the order of
    // gamedata/specialmode.staticinfoheader in package 0008, identical on 2949
    // and 2976. The component keeps a mode's ROW here, not its key: the loader
    // at 0x0184C980 puts its loop counter into the name hash at node +0x08
    // (0x00390D87: movzx eax, word ptr [rax]; mov word ptr [rbx+8], ax),
    // sizes the table to the header's count, and 0x004A2890 bounds the id
    // against that count. The first build of this check matched keys, which
    // would have refused Detect_Lantern (row 3, key 103) as Knowledge (key 3)
    // and SwordFlash (row 6) as FindCollect, and turned the flash off.
    //
    // FlashActive took any nonzero +0x40 as the flash. It is the tail of
    // whichever mode is running, and GitHub #1 is myst0ne getting pins while
    // talking to a questgiver after 1.1.21 fixed the freed component. So a
    // mode with nothing to do with detection no longer counts. Only
    // Detect_Lantern and SwordFlash define a DetectMode section with the sign
    // effects that light glints, and the data does not say which of the two
    // Blinding Flash is, so the whole Detect family stays accepted, and so does
    // any row past the table. Refused are the sixteen others, Knowledge (row
    // 8) among them, whose record draws _renderPassKnowledgeNPC over NPCs.
    struct Mode
    {
        const char* name;
        bool detection;
    };
    constexpr Mode kModes[] = {
        {"Detect", true},                           // row 0, key 1
        {"Detect_Damian", true},                    // row 1, key 11
        {"Detect_Oongka", true},                    // row 2, key 12
        {"Detect_Lantern", true},                   // row 3, key 103
        {"Detect_InteractionAim_NoLantern", true},  // row 4, key 110
        {"Detect_Ship", true},                      // row 5, key 104
        {"SwordFlash", true},                       // row 6, key 105
        {"Anamorphic", false},                      // row 7, key 2
        {"Knowledge", false},                       // row 8, key 3
        {"Hacking", false},                         // row 9, key 4
        {"AnimalTracking", false},                  // row 10, key 5
        {"FindCollect", false},                     // row 11, key 6
        {"FindMine", false},                        // row 12, key 10
        {"ReadMemory", false},                      // row 13, key 102
        {"ReadMemory_NotMoveLimit", false},         // row 14, key 109
        {"DetectTaeguk", true},                     // row 15, key 101
        {"DetectTaeguk_Damian", true},              // row 16, key 111
        {"DetectTaeguk_Oongka", true},              // row 17, key 112
        {"Jijeongta", false},                       // row 18, key 7
        {"Housing", false},                         // row 19, key 9
        {"Housing_Island", false},                  // row 20, key 24
        {"Pond", false},                            // row 21, key 20
        {"FactionManagement", false},               // row 22, key 21
        {"FactionManagementWithoutHousing", false}, // row 23, key 23
        {"MiniGameFake", false},                    // row 24, key 107
        {"RemoteCatchControl", false},              // row 25, key 108
    };
    static_assert(sizeof(kModes) / sizeof(kModes[0]) == gs::sig::kSpecialModeRows, "one entry per row");

    const Mode* FindMode(uint16_t row)
    {
        return row < sizeof(kModes) / sizeof(kModes[0]) ? &kModes[row] : nullptr;
    }

    // Zero until the worker has read the game's own table, and the check
    // stays off unless it reads kSpecialModeRows: a patch that adds or moves a
    // mode must cost the check, never the flash.
    std::atomic<uint32_t> g_modeRows{0};

    std::atomic<uint32_t> g_lastRefused{0xFFFFFFFF};
    std::atomic<int> g_refusedLogsLeft{20};

    // How far into each object it is safe to look, from the disassembly of
    // their allocation sites and their own code:
    //
    //   FindDetectTargetTask          0x70 bytes, proven. The literal size at
    //                                 its one allocation site, inside
    //                                 ClientDetectActorComponent's constructor
    //                                 at 0x00992A7D1.
    //   ClientSpecialModeActorComponent
    //                                 0xF8 bytes, proven the same way at
    //                                 0x008350B8, and its own code never
    //                                 touches past +0xF1.
    //   ClientDetectActorComponent    allocation site not found, but its
    //                                 constructor and every non-stub vtable
    //                                 slot stop at +0x250, two functions
    //                                 agreeing on the same boundary.
    //
    // The scan used to run to 0x800 on all three, so most of what it ever
    // found belonged to whatever allocation sat next in the heap. That is
    // where session nineteen's frozen actor at +0x540 came from, and session
    // fifty-three's wandering actor at detect+0x508, and the detect scalars
    // that read a world coordinate where a distance was supposed to be.
    constexpr size_t kBytes_Detect  = 0x260;
    constexpr size_t kBytes_Special = 0x100;
    constexpr size_t kBytes_Task    = 0x70;

    std::atomic<uintptr_t> g_player{0};
    std::atomic<uintptr_t> g_detect{0};
    std::atomic<uintptr_t> g_special{0};
    // What the special component looked like when it was handed over: its
    // vtable, and the actor it belongs to at +0x08. A load frees the object,
    // and this module used to go on reading the flash flag out of whatever
    // the game put in that block next. LuxDragon's 1.1.20 log: the block
    // became something else with 0xFFFFFFFF at +0x40, the flash read as on
    // for fourteen minutes, and the automatic marker pinned whatever the
    // camera settled on during a conversation and again during a relic
    // pickup. So both are recorded here, checked on every read, and a block
    // that no longer matches is dropped on the spot.
    std::atomic<uintptr_t> g_specialVt{0};
    std::atomic<uintptr_t> g_specialOwner{0};
    int g_staleLogsLeft = 4;
    int g_describeLeft = 8;
    uint64_t g_press = 0;

    uintptr_t Deref(uintptr_t at)
    {
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), sizeof(uintptr_t))) return 0;
        return *reinterpret_cast<const uintptr_t*>(at);
    }

    const char* NameOf(uintptr_t obj)
    {
        const uintptr_t vt = Deref(obj);
        return vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
    }

    constexpr uintptr_t kOff_Comp_Owner = 0x08;   // component -> the actor it belongs to

    // The component's vtable and owner, plain data out, or false when the
    // block cannot be read. Its own frame so it can hold the handler: the
    // game frees on other threads, and Readable only says what was true a
    // moment ago.
    bool ReadHeader(uintptr_t comp, uintptr_t* vt, uintptr_t* owner)
    {
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(comp), kOff_Special_Active + 4)) return false;
            *vt = *reinterpret_cast<const uintptr_t*>(comp);
            *owner = *reinterpret_cast<const uintptr_t*>(comp + kOff_Comp_Owner);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The special component while it is still the object it was when it was
    // handed over, else 0. Two reads a call, which is nothing against the
    // tick, and it is what stops a freed block being read as the flash.
    uintptr_t LiveSpecial()
    {
        const uintptr_t sp = g_special.load();
        if (!sp) return 0;
        uintptr_t vt = 0, owner = 0;
        const bool readable = ReadHeader(sp, &vt, &owner);
        const uintptr_t wantVt = g_specialVt.load();
        if (readable && vt == wantVt && owner == g_specialOwner.load()) return sp;
        // Dropped only if nobody has replaced it in the meantime: the worker
        // hands a new one over on its own thread, and the compare above can
        // fail against a pointer that has just been swapped out from under it.
        uintptr_t expected = sp;
        if (g_special.compare_exchange_strong(expected, static_cast<uintptr_t>(0)) && g_staleLogsLeft > 0)
        {
            --g_staleLogsLeft;
            GS_LOG("[aim] the special mode component at 0x%p is %s; the flash reads as off until it "
                   "is found again", reinterpret_cast<void*>(sp),
                   !readable      ? "no longer readable"
                   : vt != wantVt ? "carrying a different vtable, so the block has been reused"
                                  : "owned by a different actor, so the block belongs to another "
                                    "character now");
        }
        return 0;
    }

    bool IsActorClass(const char* n)
    {
        // Every actor class in this binary ends in "Actor@pa@@" or has "Actor@"
        // in its namespace tail; components end in "ActorComponent@pa@@" and
        // must not count.
        if (!n) return false;
        if (strstr(n, "ActorComponent@")) return false;
        return strstr(n, "Actor@") != nullptr;
    }

    bool PositionOf(uintptr_t actor, float* out)
    {
        __try
        {
            const uintptr_t comps = Deref(actor + kOff_Ent_Comps);
            if (!comps) return false;
            const uintptr_t tf = Deref(comps + kOff_Comps_Transform);
            if (!tf || !gs::rtti::Readable(reinterpret_cast<const void*>(tf), kOff_Tf_ParentPos + 12)) return false;
            float v[3], pw[3];
            memcpy(v, reinterpret_cast<const void*>(tf + kOff_Tf_Pos), sizeof(v));
            const uint32_t parent = *reinterpret_cast<const uint32_t*>(tf + kOff_Tf_ParentEid);
            if (parent != 0xFFFFFFFF && parent != 0)
            {
                memcpy(pw, reinterpret_cast<const void*>(tf + kOff_Tf_ParentPos), sizeof(pw));
                if (std::isfinite(pw[0]) && std::isfinite(pw[1]) && std::isfinite(pw[2]))
                { v[0] += pw[0]; v[1] += pw[1]; v[2] += pw[2]; }
            }
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
            if (std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[2]) > 1.0e6f) return false;
            out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The target is not a pointer to an actor: session sixteen's task held the
    // player, a Havok simulation, and nothing else with RTTI. In this engine
    // things are keyed by actor id, A0100001 style, so the target is a dword.
    // Dump the whole object so two presses, one aimed and one not, give the
    // field by diff, and flag every dword shaped like an id on the way.
    bool LooksLikeActorId(uint32_t v)
    {
        const uint32_t top = v >> 24;
        return (top == 0xA0 || top == 0xB0 || top == 0xA1 || top == 0xB1) && (v & 0x00FFFFFF) != 0;
    }

    void DumpObject(const char* tag, uintptr_t obj, size_t bytes)
    {
        size_t n = bytes;
        while (n >= 0x40 && !gs::rtti::Readable(reinterpret_cast<const void*>(obj), n)) n /= 2;
        if (n < 0x40) return;
        const auto* q = reinterpret_cast<const uint32_t*>(obj);
        for (size_t off = 0; off + 32 <= n; off += 32)
        {
            const size_t i = off / 4;
            GS_LOG("[dump %s +%03zX] %08X %08X %08X %08X %08X %08X %08X %08X", tag, off,
                   q[i], q[i+1], q[i+2], q[i+3], q[i+4], q[i+5], q[i+6], q[i+7]);
        }
        for (size_t off = 0; off + 4 <= n; off += 4)
        {
            const uint32_t v = q[off / 4];
            if (LooksLikeActorId(v)) GS_LOG("[dump %s] actor id shaped dword at +0x%03zX: %08X", tag, off, v);
        }
    }

    void DumpHeader(const char* tag, uintptr_t obj, size_t bytes) { DumpObject(tag, obj, bytes); }

    // Walk one object's pointer fields. Fills out on the first actor that is
    // not the player. Depth 1 also searches pointees that look like holders:
    // the detect component keeps a FindDetectTargetTask at +0x1D0, and the
    // target is in the task, not on the component. Plain data in and out so
    // the frame can hold the handler.
    bool Search(uintptr_t obj, uintptr_t player, bool describe, int depth, const char* tag,
                gs::aim::Target* out)
    {
        __try
        {
            if (!obj || !gs::rtti::Readable(reinterpret_cast<const void*>(obj), 0x40)) return false;
            // Bounded like the walk above, and for the same reason: this
            // function is what read past the end of these objects for the whole
            // project. It has no callers left.
            size_t bytes = kBytes_Detect;
            while (bytes > 0x40 && !gs::rtti::Readable(reinterpret_cast<const void*>(obj), bytes)) bytes /= 2;

            for (uintptr_t off = 0x08; off + 8 <= bytes; off += 8)
            {
                const uintptr_t p = *reinterpret_cast<const uintptr_t*>(obj + off);
                if (p < 0x10000 || (p & 7) != 0 || p == player || p == obj) continue;
                const char* n = NameOf(p);
                if (!n) continue;
                if (describe) GS_LOG("[aim]   %s+0x%03llX -> 0x%p %s", tag,
                                     static_cast<unsigned long long>(off), reinterpret_cast<void*>(p), n);
                if (IsActorClass(n))
                {
                    float v[3];
                    if (!PositionOf(p, v)) continue;
                    out->x = v[0]; out->y = v[1]; out->z = v[2];
                    out->actor = p;
                    out->foundAt = off;
                    strncpy_s(out->cls, sizeof(out->cls), n, _TRUNCATE);
                    out->valid = true;
                    return true;
                }
                // One level down into anything that could hold a result.
                if (depth > 0 && (strstr(n, "Task") || strstr(n, "Target") || strstr(n, "Detect") ||
                                  strstr(n, "IRefCounted")))
                {
                    if (describe && strstr(n, "FindDetectTargetTask")) DumpHeader("task", p, 0x300);
                    if (Search(p, player, describe, depth - 1, "task", out)) return true;
                }
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

namespace gs::aim
{
    void SetPlayerActor(uintptr_t actor) { g_player.store(actor); }
    void SetDetectComponent(uintptr_t comp) { g_detect.store(comp); }
    void SetSpecialComponent(uintptr_t comp)
    {
        uintptr_t vt = 0, owner = 0;
        if (comp && !ReadHeader(comp, &vt, &owner)) comp = 0;
        // The shape first and the pointer last, so a reader on the tick never
        // sees the new pointer against the old shape.
        g_specialVt.store(vt);
        g_specialOwner.store(owner);
        g_special.store(comp);
    }
    uintptr_t DetectComponent() { return g_detect.load(); }
    uintptr_t SpecialComponent() { return LiveSpecial(); }

    bool FlashActive()
    {
        const uintptr_t sp = LiveSpecial();
        if (!sp) return false;
        // Still the same object, but the player has moved on: after a load
        // the actor manager hands over the new body while this component
        // still belongs to the old one, and until it is found again the
        // honest answer is off. Not dropped, because the worker sets this
        // pointer a moment before the player walk updates the actor, and
        // that order must not throw a component away that was just found.
        const uintptr_t player = g_player.load();
        if (player && g_specialOwner.load() != player) return false;
        const uintptr_t at = sp + kOff_Special_Active;
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(sp + kOff_Special_Mode),
                                kOff_Special_Active + 4 - kOff_Special_Mode))
            return false;
        if (*reinterpret_cast<const uint32_t*>(at) == 0) return false;
        if (g_modeRows.load() != gs::sig::kSpecialModeRows) return true;
        const uint16_t id = *reinterpret_cast<const uint16_t*>(sp + kOff_Special_Mode);
        const Mode* m = FindMode(id);
        if (!m || m->detection)
        {
            g_lastRefused.store(0xFFFFFFFF);   // so the next refusal is said again
            return true;
        }
        // Said once each time the refused mode changes, so a conversation
        // shows up as one line and a wrong call here is plain in a report.
        if (g_lastRefused.exchange(id) != id && g_refusedLogsLeft.fetch_sub(1) > 0)
            GS_LOG("[flash] the special mode flag is up for %s (row %u), which is not a detect mode, so the "
                   "automatic marker does not treat it as Blinding Flash", m->name, static_cast<unsigned>(id));
        return false;
    }

    const char* ModeName(uint16_t row)
    {
        const Mode* m = FindMode(row);
        return m ? m->name : nullptr;
    }

    void SetModeTableRows(uint32_t rows) { g_modeRows.store(rows); }

    // For the log, when the flag goes up. FlashActive reads only +0x40, the
    // tail of the first record, and never asks which mode is in it. GitHub #1
    // has myst0ne getting pins while talking to a questgiver on 1.1.22, after
    // the stale-flash fix, and a conversation that runs as a special mode of
    // its own would look exactly like the flash here. The mode id says which.
    bool ModeRecords(uint16_t* first, uint16_t* second, uint32_t* firstTail, uint32_t* secondTail)
    {
        const uintptr_t sp = LiveSpecial();
        if (!sp) return false;
        __try
        {
            *first = *reinterpret_cast<const uint16_t*>(sp + 0x30);
            *second = *reinterpret_cast<const uint16_t*>(sp + 0x48);
            *firstTail = *reinterpret_cast<const uint32_t*>(sp + 0x40);
            *secondTail = *reinterpret_cast<const uint32_t*>(sp + 0x58);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    namespace
    {
        // One actor pointer found on one of the detect objects, resolved and
        // described. The transform does not say which frame its fields are in,
        // so both are computed and the one that lands within a kilometre and a
        // half of the player wins, the way actors.cpp does it. Session
        // nineteen's target read (-835.600, 536.055, -299.897), which is the
        // sub-level frame; the world is that plus the origin.
        bool Describe(const char* base, uintptr_t off, uintptr_t p, const char* cls,
                      const gs::aim::Eye& eye, bool log, gs::aim::Held* out)
        {
            __try
            {
                constexpr uintptr_t kEid = 0x60, kComps = 0x68, kTf = 0x1A0;
                constexpr uintptr_t kWorld = 0x29C, kLocal = 0xB4, kParentEid = 0xC8, kParentPos = 0xEC;
                const uint32_t eid = gs::rtti::Readable(reinterpret_cast<const void*>(p + kEid), 4)
                                         ? *reinterpret_cast<const uint32_t*>(p + kEid)
                                         : 0;
                const uintptr_t comps = Deref(p + kComps);
                const uintptr_t tf = comps ? Deref(comps + kTf) : 0;
                if (!tf || !gs::rtti::Readable(reinterpret_cast<const void*>(tf), kWorld + 12))
                {
                    if (log) GS_LOG("[target] %s+0x%03llX  %s  eid %08X  no transform", base,
                                    static_cast<unsigned long long>(off), cls, eid);
                    return false;
                }
                float w[3], l[3], pw[3];
                memcpy(w, reinterpret_cast<const void*>(tf + kWorld), 12);
                memcpy(l, reinterpret_cast<const void*>(tf + kLocal), 12);
                memcpy(pw, reinterpret_cast<const void*>(tf + kParentPos), 12);
                const uint32_t parent = *reinterpret_cast<const uint32_t*>(tf + kParentEid);
                const float gx = eye.px - eye.lx, gy = eye.py - eye.ly, gz = eye.pz - eye.lz;

                struct Cand { float p[3]; const char* how; };
                Cand cands[3] = {
                    {{w[0], w[1], w[2]}, "its cached world position"},
                    {{l[0] + gx, l[1] + gy, l[2] + gz}, "its local position and the sub-level origin"},
                    {{l[0] + pw[0], l[1] + pw[1], l[2] + pw[2]}, "its local position and its parent"},
                };
                int pick = -1;
                float pickDist = 0;
                for (int i = 0; i < 3; ++i)
                {
                    if (!std::isfinite(cands[i].p[0]) || !std::isfinite(cands[i].p[2])) continue;
                    const float dx = cands[i].p[0] - eye.px, dz = cands[i].p[2] - eye.pz;
                    const float d = std::sqrt(dx * dx + dz * dz);
                    if (d > 1500.0f) continue;
                    if (pick < 0 || d < pickDist) { pick = i; pickDist = d; }
                }
                const bool live = gs::actors::InSet(p);
                if (log)
                {
                    GS_LOG("[target] %s+0x%03llX  %s  eid %08X  %s", base,
                           static_cast<unsigned long long>(off), cls, eid,
                           live ? "the pools hold it, so it is live"
                                : "the pools do not hold it, so it is stale or not an actor");
                    GS_LOG("[target]   cached world (%.1f, %.1f, %.1f)  local (%.1f, %.1f, %.1f)  "
                           "parent eid %08X at (%.1f, %.1f, %.1f)",
                           w[0], w[1], w[2], l[0], l[1], l[2], parent, pw[0], pw[1], pw[2]);
                }
                if (pick < 0)
                {
                    if (log) GS_LOG("[target]   no frame puts it within a kilometre and a half of you");
                    return false;
                }
                const float* q = cands[pick].p;
                float angle = 180.0f;
                const float ax = q[0] - eye.ox, az = q[2] - eye.oz;
                const float f = std::sqrt(ax * ax + az * az);
                if (f > 0.5f)
                    angle = std::fabs(std::atan2((ax * eye.uz - az * eye.ux) / f,
                                                 (ax * eye.ux + az * eye.uz) / f)) * 57.2958f;
                if (log)
                    GS_LOG("[target]   %s puts it at (%.1f, %.1f, %.1f), %.1f m away, %.1f degrees off "
                           "the crosshair", cands[pick].how, q[0], q[1], q[2], pickDist, angle);
                // A live actor always beats a stale one, and among equals the
                // one nearest the crosshair wins.
                const bool better = out && (!out->valid || (live && !out->inPools) ||
                                            (live == out->inPools && angle < out->angle));
                if (better)
                {
                    out->x = q[0]; out->y = q[1]; out->z = q[2];
                    out->dist = pickDist;
                    out->angle = angle;
                    out->eid = eid;
                    out->at = off;
                    out->inPools = live;
                    strncpy_s(out->where, sizeof(out->where), base, _TRUNCATE);
                    strncpy_s(out->cls, sizeof(out->cls), cls, _TRUNCATE);
                    out->valid = true;
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Every actor pointer in one object, one level down into a task.
        int Walk(const char* base, uintptr_t obj, uintptr_t player, int depth,
                 size_t limit, const gs::aim::Eye& eye, bool log, gs::aim::Held* out)
        {
            int found = 0;
            __try
            {
                if (!obj || !gs::rtti::Readable(reinterpret_cast<const void*>(obj), 0x40)) return 0;
                size_t bytes = limit;
                while (bytes > 0x40 && !gs::rtti::Readable(reinterpret_cast<const void*>(obj), bytes)) bytes /= 2;
                for (uintptr_t off = 0x08; off + 8 <= bytes; off += 8)
                {
                    const uintptr_t p = *reinterpret_cast<const uintptr_t*>(obj + off);
                    if (p < 0x10000 || (p & 7) != 0 || p == player || p == obj) continue;
                    const char* n = NameOf(p);
                    if (!n) continue;
                    if (IsActorClass(n))
                    {
                        Describe(base, off, p, n, eye, log, out);
                        if (++found >= 16) return found;
                        continue;
                    }
                    if (depth > 0 && strstr(n, "FindDetectTargetTask"))
                        found += Walk("task", p, player, depth - 1, kBytes_Task, eye, log, out);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return found;
        }

        float ScalarAt(uintptr_t obj, uintptr_t off)
        {
            if (!obj || !gs::rtti::Readable(reinterpret_cast<const void*>(obj + off), 4)) return 0.0f;
            float v;
            __try { memcpy(&v, reinterpret_cast<const void*>(obj + off), 4); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
            return v;
        }

        uint32_t DwordAt(uintptr_t obj, uintptr_t off)
        {
            if (!obj || !gs::rtti::Readable(reinterpret_cast<const void*>(obj + off), 4)) return 0;
            uint32_t v;
            __try { memcpy(&v, reinterpret_cast<const void*>(obj + off), 4); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
            return v;
        }
    }

    Held DescribeTargets(const Eye& eye, bool log)
    {
        Held best;
        const uintptr_t detect = g_detect.load();
        const uintptr_t special = LiveSpecial();
        const uintptr_t player = g_player.load();
        // The scalars an older build's notes named live past +0x250, which is
        // where this class's own code stops, so they are somebody else's
        // memory. Session fifty-three read a world coordinate at +0x3E8 and
        // 0x86322B60 at the flag that is supposed to be zero or 0xFF. They are
        // printed from inside the object's real bounds only.
        if (log)
            GS_LOG("[target] detect +1D0 task 0x%p, +1DA %u, +208 %.3f, +240 %.3f",
                   reinterpret_cast<void*>(DetectTask()), DwordAt(detect, 0x1D8) >> 16,
                   ScalarAt(detect, 0x208), ScalarAt(detect, 0x240));
        int n = Walk("detect", detect, player, 1, kBytes_Detect, eye, log, &best);
        n += Walk("special", special, player, 1, kBytes_Special, eye, log, &best);
        if (!n && log) GS_LOG("[target] no actor pointer on either component or the task right now");
        return best;
    }

    Target Resolve()
    {
        Target t;
        const uintptr_t player = g_player.load();
        const uintptr_t detect = g_detect.load();
        const uintptr_t special = LiveSpecial();
        const bool describe = g_describeLeft > 0;
        if (describe) --g_describeLeft;

        ++g_press;
        if (describe) GS_LOG("[aim] press %llu: detect component 0x%p, special 0x%p, player 0x%p, flash %s",
                             static_cast<unsigned long long>(g_press),
                             reinterpret_cast<void*>(detect), reinterpret_cast<void*>(special),
                             reinterpret_cast<void*>(player), FlashActive() ? "on" : "off");
        if (describe && detect) DumpObject("detect", detect, 0x800);
        if (describe && special) DumpObject("special", special, 0x400);

        if (detect && Search(detect, player, describe, 1, "detect", &t))
        {
            GS_LOG_OK("[aim] target via detect component +0x%llX: %s at (%.3f, %.3f, %.3f)",
                      static_cast<unsigned long long>(t.foundAt), t.cls, t.x, t.y, t.z);
            return t;
        }
        if (special && Search(special, player, describe, 1, "special", &t))
        {
            GS_LOG_OK("[aim] target via special component +0x%llX: %s at (%.3f, %.3f, %.3f)",
                      static_cast<unsigned long long>(t.foundAt), t.cls, t.x, t.y, t.z);
            return t;
        }
        GS_LOG("[aim] no actor pointer in either component right now");
        return t;
    }

    bool AimPointLocal(uintptr_t charctl, float lx, float ly, float lz, float* out)
    {
        constexpr uintptr_t kOff_CharCtl_Aim = 0x318;
        if (!charctl) return false;
        const uintptr_t at = charctl + kOff_CharCtl_Aim;
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 12)) return false;
        float f[3];
        __try
        {
            memcpy(f, reinterpret_cast<const void*>(at), 12);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) return false;
        const float dx = f[0] - lx, dy = f[1] - ly, dz = f[2] - lz;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < 0.25f || d > 150.0f) return false;
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
        return true;
    }

    bool DetectDistance(float* out)
    {
        const uintptr_t d = g_detect.load();
        if (!d) return false;
        const uintptr_t at = d + 0x3EC;
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 4)) return false;
        float v;
        __try { memcpy(&v, reinterpret_cast<const void*>(at), 4); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!std::isfinite(v) || v > 1.0e6f || v <= 0.0f) return false;
        *out = v;
        return true;
    }

    uintptr_t DetectTask()
    {
        const uintptr_t detect = g_detect.load();
        if (!detect) return 0;
        // Session sixteen: the task sits at +0x1D0. Verified by name before use.
        const uintptr_t p = Deref(detect + 0x1D0);
        const char* n = p ? NameOf(p) : nullptr;
        return (n && strstr(n, "FindDetectTargetTask")) ? p : 0;
    }
}
