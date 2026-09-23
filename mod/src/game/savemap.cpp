#include "game/savemap.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/load.h"
#include "core/log.h"
#include "core/pinstore.h"
#include "game/actors.h"
#include "game/lgso.h"
#include "game/pickup.h"
#include "game/player.h"
#include "game/rtti.h"
#include "game/typescan.h"

namespace
{
    constexpr size_t kRecordBytes = 0x3D8;            // the stride of the records in their vector
    constexpr uintptr_t kOff_FieldRecords = 0x30;     // FieldSaveData's first vector, as its copy builds it
    constexpr size_t kOff_Pos = 0x1E8, kOff_OriginPos = 0x210, kOff_State = 0x21C;
    // A record joins a placement within three metres across and six up or
    // down. The height is looser because the thing that glints can sit on
    // the placement: on 22 September the Complete record for an artifact
    // taken from an abyss cresset was 0.6 metres across from the cresset's
    // placement and 3.1 above it, and a three metre sphere missed it.
    constexpr float kJoinMetres = 3.0f, kJoinHeight = 6.0f;

    // Jenkins lookup3 of the lowercase name, as statehash.py computes them.
    constexpr uint32_t kClear = 0xE300ACFE, kComplete = 0xA4D24DF1, kCompleted = 0x03ED0175;
    struct StateName { uint32_t hash; const char* name; };
    const StateName kStates[] = {
        {0x866C7489, "Wait"},     {0x150B14D0, "GimmickOn"}, {0x6AFC553C, "Lock"},
        {0xA488AC26, "Deactive"}, {kClear, "Clear"},         {0x353C1CAD, "Break"},
        {0x64E69941, "Broken"},   {0x4C4B2355, "Open"},      {0x9F3CC1C9, "Opened"},
        {0x92D1FA8F, "Close"},    {0x5D484706, "Closed"},    {kComplete, "Complete"},
        {kCompleted, "Completed"}, {0x6BBA3E96, "Active"},   {0x89495137, "Dead"},
        {0x8C2664A3, "Used"},     {0xF177B780, "Empty"},     {0xB9C3379D, "PreWait"},
        {0xC7D8C27C, "PreGimmickOn"},
    };
    // A collection gimmick goes to Clear when taken; a puzzle that is done
    // has nothing left to glint either.
    bool Taken(uint32_t h) { return h == kClear || h == kComplete || h == kCompleted; }

    // A teleporter is done once it is switched on, and switched on is
    // GimmickOn. Each ruin is two gimmicks on one AbyssRuins_ placement, the
    // use-artifact one and its part, so the save holds two records on it. On
    // 23 September the six Hernand ruins holding a GimmickOn record were
    // exactly the six the map drew as MapIcon_Abyss_Ruins, at the same
    // positions, and the 207 with no record had no icon. Only for those
    // placements: GimmickOn on anything else means nothing about being done.
    constexpr uint32_t kGimmickOn = 0x150B14D0;
    bool Ruin(const char* placeName) { return strncmp(placeName, "AbyssRuins_", 11) == 0; }
    const char* NameOf(uint32_t h)
    {
        for (const StateName& s : kStates) if (s.hash == h) return s.name;
        return nullptr;
    }

    std::atomic<bool> g_started{false};
    // The thread, kept so Stop can wait for it, and the event that ends its
    // wait between reads. An unload that left it running would have it wake
    // into unmapped code.
    HANDLE g_thread = nullptr;
    HANDLE g_stopEvent = nullptr;

    // Waits between reads; false once Stop has been asked for.
    bool Pause(DWORD ms)
    {
        return WaitForSingleObject(g_stopEvent, ms) == WAIT_TIMEOUT;
    }
    std::mutex g_mutex;
    // Placement addresses the save has had as taken. It only grows until the
    // save or the table changes: a taken item does not come back short of a
    // load, and a read that misses a field for a second, while the game is
    // changing its vector, must not hand the glint back for that second.
    std::unordered_set<uintptr_t> g_taken;
    // Where each of them stands, so a pin can ask whether the thing under it
    // has gone without knowing which placement it was.
    struct Spot { float x, y, z; };
    std::vector<Spot> g_takenAt;
    // Placement address -> the saved states on it, and -> the live state of
    // the loaded gimmick standing on it, both for the log. Under g_mutex.
    std::unordered_map<uintptr_t, std::string> g_saveStates;
    std::unordered_map<uintptr_t, uint32_t> g_liveByPlace;
    uintptr_t g_takenComp = 0;    // the component the set was read from
    uint32_t g_takenGen = 0;      // and the table generation
    uintptr_t g_lastComp = 0;     // the component Fields last reached

    uintptr_t g_base = 0;

    // The classes the route passes through, as recorded on 2976. Each is held
    // up against the running game's RTTI and found again by name when a patch
    // has moved it, which every patch so far has.
    struct Cls { uintptr_t rva; const char* name; uintptr_t va; };
    Cls g_cls[] = {
        {0x058B0B20, ".?AVFieldGimmickSaveData@pa@@", 0},
        {0x058AF0E0, ".?AVFieldSaveData@pa@@", 0},
        {0x05B25988, ".?AVServerActorManager@pa@@", 0},
        {0x05B257A8, ".?AVServerUserActor@pa@@", 0},
        {0x05B1D318, ".?AVServerContentsMiscActorComponent@pa@@", 0},
    };
    enum { kRecord, kField, kServerManager, kServerUser, kContentsMisc, kClassCount };
    uintptr_t Vt(int i) { return g_cls[i].va; }

    bool ResolveClasses()
    {
        int stale = 0;
        for (Cls& c : g_cls)
        {
            c.va = gs::rtti::VtableIs(reinterpret_cast<const void*>(g_base + c.rva), c.name) ? g_base + c.rva : 0;
            if (!c.va) ++stale;
        }
        if (stale)
        {
            const char* kw[kClassCount];
            for (int i = 0; i < kClassCount; ++i) kw[i] = g_cls[i].name;
            static gs::typescan::ClassInfo found[16];
            const size_t n = gs::typescan::FindClasses(kw, kClassCount, found, 16);
            for (Cls& c : g_cls)
                for (size_t i = 0; i < n && !c.va; ++i)
                    if (found[i].vtableVa && strcmp(found[i].name, c.name) == 0) c.va = found[i].vtableVa;
        }
        for (const Cls& c : g_cls)
        {
            if (c.va) continue;
            GS_LOG_ERR("[savemap] RTTI does not offer %s, so taken glints are not filtered", c.name);
            return false;
        }
        if (stale)
            GS_LOG_OK("[savemap] %d of %d save classes had moved and RTTI found each one again", stale, kClassCount);
        return true;
    }
    std::vector<gs::lgso::Place> g_places;
    std::vector<uintptr_t> g_fields;          // FieldSaveData objects
    std::unordered_map<uintptr_t, uint32_t> g_lastState;   // record address -> state last seen
    int g_linesLeft = 2000;

    bool Say()
    {
        if (g_linesLeft <= 0) return false;
        --g_linesLeft;
        return true;
    }

    // The handler is the guard, with no VirtualQuery first. A query per
    // record read every second was most of the 3,600 a second the 23
    // September load report counted, and each one takes the lock the game's
    // own allocations take. actors.cpp and the table reader read the same way.
    bool CopyOut(uintptr_t at, void* out, size_t n)
    {
        if (at < 0x10000 || at > 0x00007FFFFFFFFFFFull) return false;
        __try
        {
            memcpy(out, reinterpret_cast<const void*>(at), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uintptr_t Deref(uintptr_t at)
    {
        uintptr_t v = 0;
        return CopyOut(at, &v, 8) ? v : 0;
    }

    // ---- the table, and a grid over it for the join ----

    std::unordered_map<int64_t, std::vector<int>> g_grid;
    int64_t Cell(float x, float z)
    {
        return (static_cast<int64_t>(std::floor(x / 16.0f)) << 32) ^
               static_cast<int64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::floor(z / 16.0f))));
    }
    // The table generation g_places was copied from. Placements are matched
    // by address, so a copy from an older table matches nothing tick.cpp asks
    // about; the generation, not the count, says when to copy again.
    uint32_t g_placesGen = 0;

    void LoadPlaces()
    {
        g_places.resize(40000);
        g_places.resize(static_cast<size_t>(gs::lgso::CopyAll(g_places.data(), 40000, &g_placesGen)));
        g_grid.clear();
        for (int i = 0; i < static_cast<int>(g_places.size()); ++i)
            g_grid[Cell(g_places[i].x, g_places[i].z)].push_back(i);
    }
    int Nearest(const float* p, float* dist)
    {
        int best = -1;
        float bd = kJoinMetres * kJoinMetres;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz)
            {
                auto it = g_grid.find(Cell(p[0] + dx * 16.0f, p[2] + dz * 16.0f));
                if (it == g_grid.end()) continue;
                for (int i : it->second)
                {
                    const gs::lgso::Place& q = g_places[i];
                    const float ex = q.x - p[0], ez = q.z - p[2];
                    if (std::fabs(q.y - p[1]) > kJoinHeight) continue;
                    const float d = ex * ex + ez * ez;
                    if (d < bd) { bd = d; best = i; }
                }
            }
        if (dist) *dist = std::sqrt(bd);
        return best;
    }

    // ---- one record ----

    struct Rec
    {
        uintptr_t at = 0;
        float pos[3] = {};
        bool hasPos = false;
        uint32_t state = 0;
        int place = -1;
        float dist = 0;
    };

    bool PosOf(const uint8_t* b, size_t off, float* p)
    {
        memcpy(p, b + off, 12);
        for (int i = 0; i < 3; ++i)
            if (!(p[i] == p[i]) || std::fabs(p[i]) > 1.0e6f) return false;
        return std::fabs(p[0]) > 1.0f || std::fabs(p[2]) > 1.0f;
    }

    bool ReadRec(uintptr_t at, Rec& r)
    {
        uint8_t b[kOff_State + 4];
        if (!CopyOut(at, b, sizeof(b))) return false;
        uintptr_t vt;
        memcpy(&vt, b, 8);
        if (vt != Vt(kRecord)) return false;
        r.at = at;
        r.hasPos = PosOf(b, kOff_OriginPos, r.pos) || PosOf(b, kOff_Pos, r.pos);
        memcpy(&r.state, b + kOff_State, 4);
        r.place = r.hasPos ? Nearest(r.pos, &r.dist) : -1;
        return true;
    }

    // ---- where the records are ----

    // A FieldSaveData's record vector: a pointer at +0x30 and a u32 count at
    // +0x38, which is how the load routine walks it (ServerContentsMisc slot
    // 10 on 2949, 0x028B8CCF: rsi = [r12+0x30], end = rsi + [r12+0x38] * 0x3D8).
    // Checked by the vtable of the first and last record, so a wrong reading
    // of the layout gives nothing rather than rubbish.
    bool FieldRecords(uintptr_t field, uintptr_t* begin, size_t* count)
    {
        uintptr_t ptr = 0;
        uint32_t n = 0;
        if (!CopyOut(field + kOff_FieldRecords, &ptr, 8) || !CopyOut(field + kOff_FieldRecords + 8, &n, 4))
            return false;
        if (!ptr || !n) { *begin = 0; *count = 0; return ptr == 0 || n == 0; }
        if (n > 200000) return false;
        if (Deref(ptr) != Vt(kRecord) ||
            Deref(ptr + (static_cast<size_t>(n) - 1) * kRecordBytes) != Vt(kRecord))
            return false;
        *begin = ptr;
        *count = n;
        return true;
    }

    void ReadAll(std::vector<Rec>& out)
    {
        out.clear();
        for (uintptr_t f : g_fields)
        {
            uintptr_t begin = 0;
            size_t n = 0;
            if (!FieldRecords(f, &begin, &n)) continue;
            for (size_t i = 0; i < n; ++i)
            {
                Rec r;
                if (ReadRec(begin + i * kRecordBytes, r)) out.push_back(r);
            }
        }
    }

    void Take(int place)
    {
        const gs::lgso::Place& q = g_places[place];
        if (g_taken.insert(q.at).second) g_takenAt.push_back({q.x, q.y, q.z});
    }

    void Publish(const std::vector<Rec>& recs, const std::vector<int>& live)
    {
        // What the save holds on each placement, for the flash's candidate
        // lines to quote. Built before the lock is taken: it is a string per
        // record every second, and the flash's lookups on the game's thread
        // wait on g_mutex.
        std::unordered_map<uintptr_t, std::string> states;
        states.reserve(recs.size());
        for (const Rec& r : recs)
        {
            if (r.place < 0) continue;
            std::string& s = states[g_places[r.place].at];
            if (!s.empty()) s += ",";
            const char* nm = NameOf(r.state);
            char hex[12];
            _snprintf_s(hex, sizeof(hex), _TRUNCATE, "%08X", r.state);
            s += nm ? nm : hex;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_takenComp != g_lastComp || g_takenGen != g_placesGen)
        {
            g_taken.clear();
            g_takenAt.clear();
            g_takenComp = g_lastComp;
            g_takenGen = g_placesGen;
        }
        for (const Rec& r : recs)
            if (r.place >= 0 && (Taken(r.state) || (r.state == kGimmickOn && Ruin(g_places[r.place].name))))
                Take(r.place);
        for (int place : live) Take(place);
        g_saveStates.swap(states);
    }

    // ---- logging ----

    void Line(const char* tag, const Rec& r, uint32_t was)
    {
        const gs::player::Pos pp = gs::player::Read();
        const float ex = r.pos[0] - pp.x, ez = r.pos[2] - pp.z;
        char state[48], before[64] = "";
        const char* nm = NameOf(r.state);
        if (nm) strcpy_s(state, nm);
        else _snprintf_s(state, sizeof(state), _TRUNCATE, "%08X", r.state);
        if (was)
        {
            const char* wn = NameOf(was);
            if (wn) _snprintf_s(before, sizeof(before), _TRUNCATE, ", was %s", wn);
            else _snprintf_s(before, sizeof(before), _TRUNCATE, ", was %08X", was);
        }
        char where[200] = "no placement within three metres across";
        if (r.place >= 0)
            _snprintf_s(where, sizeof(where), _TRUNCATE, "record %u element %u \"%s\" %.1f m from it, at 0x%p",
                        g_places[r.place].record, g_places[r.place].element,
                        g_places[r.place].name[0] ? g_places[r.place].name : "unnamed", r.dist,
                        reinterpret_cast<void*>(g_places[r.place].at));
        if (Say())
            GS_LOG("[savemap] %s 0x%p at (%.1f, %.1f, %.1f), %.0f m away, state %s%s: %s", tag,
                   reinterpret_cast<void*>(r.at), r.pos[0], r.pos[1], r.pos[2],
                   pp.valid ? std::sqrt(ex * ex + ez * ez) : -1.0f, state, before, where);
    }

    // Every teleporter the table lists and what the save holds on it, once per
    // save read. The teleporter is the ruin's use-artifact gimmick, standing
    // exactly on an AbyssRuins_ placement in table record 6, and it declares
    // only Wait, GimmickOn, Clear, Lock and Deactive. Which of those means the
    // player has switched it on is the question; set against a map of the
    // unlocked ones, this list answers it.
    void SurveyTeleporters(const std::vector<Rec>& recs)
    {
        std::map<int, std::string> states;   // placement -> the states on it
        for (const Rec& r : recs)
        {
            if (r.place < 0 || strncmp(g_places[r.place].name, "AbyssRuins_", 11) != 0) continue;
            const char* nm = NameOf(r.state);
            char hex[16];
            _snprintf_s(hex, sizeof(hex), _TRUNCATE, "%08X", r.state);
            std::string& s = states[r.place];
            if (!s.empty()) s += ", ";
            s += nm ? nm : hex;
        }
        // Only the ones the save holds something on are listed one by one. The
        // first cut listed every ruin in table order and ran out at 120 before
        // it reached Hernand, the one region the test save has records for.
        std::map<std::string, int> bare;   // region -> ruins with no record
        const gs::player::Pos pp = gs::player::Read();
        for (int i = 0; i < static_cast<int>(g_places.size()); ++i)
        {
            const gs::lgso::Place& q = g_places[i];
            if (strncmp(q.name, "AbyssRuins_", 11) != 0) continue;
            auto it = states.find(i);
            if (it == states.end())
            {
                std::string region(q.name + 11);
                region = region.substr(0, region.find('_'));
                ++bare[region];
                continue;
            }
            if (!Say()) break;
            const float ex = q.x - pp.x, ez = q.z - pp.z;
            GS_LOG("[teleport] %s, record %u element %u at (%.1f, %.1f, %.1f), %.0f m away: %s", q.name, q.record,
                   q.element, q.x, q.y, q.z, pp.valid ? std::sqrt(ex * ex + ez * ez) : -1.0f, it->second.c_str());
        }
        for (const auto& kv : bare)
            if (Say()) GS_LOG("[teleport] %d %s ruin(s) with no save record", kv.second, kv.first.c_str());
    }

    void Survey(const std::vector<Rec>& recs)
    {
        SurveyTeleporters(recs);
        std::map<uint32_t, int> byState;
        std::map<std::string, int> takenByName, otherByName;
        for (const Rec& r : recs)
        {
            ++byState[r.state];
            if (r.place < 0) continue;
            const char* n = g_places[r.place].name[0] ? g_places[r.place].name : "unnamed";
            (Taken(r.state) ? takenByName : otherByName)[n]++;
        }
        if (Say()) GS_LOG("[savemap] %zu record(s), by saved state:", recs.size());
        for (const auto& kv : byState)
        {
            const char* nm = NameOf(kv.first);
            if (Say()) GS_LOG("[savemap]   %08X %s: %d", kv.first, nm ? nm : "", kv.second);
        }
        int k = 0;
        for (const auto& kv : takenByName)
            if (k++ < 60 && Say()) GS_LOG("[savemap]   taken: %d x \"%s\"", kv.second, kv.first.c_str());
        k = 0;
        for (const auto& kv : otherByName)
            if (k++ < 60 && Say()) GS_LOG("[savemap]   on a placement, not taken: %d x \"%s\"", kv.second, kv.first.c_str());
        // The nearest forty with a position, so a test can be checked by eye.
        const gs::player::Pos pp = gs::player::Read();
        std::vector<std::pair<float, size_t>> order;
        for (size_t i = 0; i < recs.size(); ++i)
        {
            if (!recs[i].hasPos || !pp.valid) continue;
            const float ex = recs[i].pos[0] - pp.x, ez = recs[i].pos[2] - pp.z;
            order.push_back({ex * ex + ez * ez, i});
        }
        std::sort(order.begin(), order.end());
        for (size_t i = 0; i < order.size() && i < 40; ++i) Line("near", recs[order[i].second], 0);
    }

    // ---- where they are, with no search ----
    //
    // The load routine is slot 10 of the player's server
    // ServerContentsMiscActorComponent (0x028B7360 on 2976). For each
    // FieldSaveData in the save it builds a copy and inserts it with 0x028EECC0
    // into a map at this+0x2C0, keyed by the field. From the insert:
    //
    //   map +0x00 u32 bucket count   +0x04 u32 entries   +0x08 u32 capacity
    //       +0x10 buckets, 0x100 bytes each of {u32 n, then {key, index} pairs}
    //       +0x18 entries, an array of node pointers
    //   node, 0xB8 bytes: +0 slot, +4 field key, +8 the FieldSaveData itself
    //
    // The component itself comes from the server actor manager's global, as
    // the 22 September wide read found: [exe+0x06DA6798] is the manager,
    // [manager+0xB8] a list whose first entry is the player's ServerUserActor,
    // and [actor+0x68]+0x368 the component. Every step is checked against its
    // vtable.
    //
    // The game fills that global at runtime and no code in the image writes it
    // with a plain move, so it cannot be read out of a new exe the way the
    // vtables can. When it does not hold the manager, the image is searched for
    // the global that does, and the log names it so the record can be brought
    // up to date. 0x06DA6798 on 2949; 0x06DA67D8 on 2976, from the search in
    // Seth's 23 September playtest.
    constexpr uintptr_t kServerManagerGlobal = 0x06DA67D8;
    constexpr int kUsersMax = 64;   // list entries asked for the component
    constexpr uintptr_t kOff_Mgr_User = 0xB8, kOff_Actor_Comps = 0x68, kOff_Comps_ContentsMisc = 0x368;
    constexpr uintptr_t kOff_FieldMap = 0x2C0, kOff_Map_Count = 0x04, kOff_Map_Entries = 0x18;
    constexpr uintptr_t kOff_Node_Field = 0x08;

    const char* g_why = "";

    // Globals the search found holding the manager, for when the recorded one
    // does not.
    uintptr_t g_mgrGlobals[4];
    int g_mgrGlobalN = 0;
    int g_mgrScans = 0;
    uint32_t g_mgrScanMs = 0;

    uintptr_t ServerManager()
    {
        const uintptr_t vt = Vt(kServerManager);
        const uintptr_t recorded = Deref(g_base + kServerManagerGlobal);
        if (recorded && Deref(recorded) == vt) return recorded;
        for (int i = 0; i < g_mgrGlobalN; ++i)
        {
            const uintptr_t m = Deref(g_mgrGlobals[i]);
            if (m && Deref(m) == vt) return m;
        }
        // Three searches at most, a minute apart. Fields is only asked once the
        // world is up, so the first one comes after the manager exists.
        const uint32_t now = GetTickCount();
        if (g_mgrScans >= 3 || (g_mgrScans && now - g_mgrScanMs < 60000)) return 0;
        ++g_mgrScans;
        g_mgrScanMs = now;
        g_mgrGlobalN = gs::typescan::FindGlobals(vt, g_mgrGlobals, 4);
        const uint32_t took = GetTickCount() - now;
        if (!g_mgrGlobalN)
        {
            GS_LOG("[savemap] no global in the image holds the ServerActorManager (searched in %u ms)", took);
            return 0;
        }
        for (int i = 0; i < g_mgrGlobalN; ++i)
            GS_LOG_OK("[savemap] the ServerActorManager is held at global +0x%llX, not the recorded +0x%llX "
                      "(searched in %u ms)", static_cast<unsigned long long>(g_mgrGlobals[i] - g_base),
                      static_cast<unsigned long long>(kServerManagerGlobal), took);
        return Deref(g_mgrGlobals[0]);
    }

    struct Candidate { uintptr_t comp; int entry; uintptr_t actorVt; };

    // Every ServerContentsMiscActorComponent hanging off an actor in the
    // manager's list, the player's first.
    //
    // +0xB8 points at a list, and the player is in it: the 15:30 wide read
    // logged "[ServerActorManager+0xB8]+0x0 holds it". Only the
    // ServerUserActor was asked, and after a save loaded mid-session that
    // stopped working. The retest on 23 September dumped the list forty
    // seconds after loading slot103: entry 0 was the ServerUserActor with
    // nothing at +0x368, and the only component was on entry 3, a
    // ServerChildOnlyInGameActor, which is also what the 22 September wide
    // read found at the component's +0x08. So every entry is asked, the
    // players before the rest, and Fields takes the first whose field map
    // holds FieldSaveData.
    int ServerComponents(Candidate* out, int cap)
    {
        const uintptr_t mgr = ServerManager();
        if (!mgr) { g_why = "no server actor manager"; return 0; }
        const uintptr_t list = Deref(mgr + kOff_Mgr_User);
        int n = 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int i = 0; list && i < kUsersMax && n < cap; ++i)
            {
                const uintptr_t a = Deref(list + 8ull * i);
                const uintptr_t avt = a ? Deref(a) : 0;
                if (!avt || (avt == Vt(kServerUser)) != (pass == 0)) continue;
                const uintptr_t comps = Deref(a + kOff_Actor_Comps);
                const uintptr_t comp = comps ? Deref(comps + kOff_Comps_ContentsMisc) : 0;
                if (comp && Deref(comp) == Vt(kContentsMisc)) out[n++] = {comp, i, avt};
            }
        }
        if (!n) g_why = list ? "no ServerContentsMiscActorComponent on any actor in the list" : "the manager has no list";
        return n;
    }

    // What the list held when the records could not be reached, so a log from
    // a failure says which step broke and on which entry.
    void DumpUsers()
    {
        const uintptr_t mgr = ServerManager();
        if (!mgr) { GS_LOG("[savemap]   no ServerActorManager in any known global"); return; }
        const uintptr_t list = Deref(mgr + kOff_Mgr_User);
        GS_LOG("[savemap]   manager 0x%p, list at +0x%llX is 0x%p", reinterpret_cast<void*>(mgr),
               static_cast<unsigned long long>(kOff_Mgr_User), reinterpret_cast<void*>(list));
        for (int i = 0; list && i < kUsersMax; ++i)
        {
            const uintptr_t a = Deref(list + 8ull * i);
            if (!a) continue;
            const uintptr_t avt = Deref(a);
            const char* an = avt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(avt)) : nullptr;
            if (!an) continue;
            const uintptr_t comps = Deref(a + kOff_Actor_Comps);
            const uintptr_t comp = comps ? Deref(comps + kOff_Comps_ContentsMisc) : 0;
            const uintptr_t cvt = comp ? Deref(comp) : 0;
            const char* cn = cvt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(cvt)) : nullptr;
            GS_LOG("[savemap]   entry %d 0x%p %s, components 0x%p, +0x%llX holds 0x%p %s", i,
                   reinterpret_cast<void*>(a), an, reinterpret_cast<void*>(comps),
                   static_cast<unsigned long long>(kOff_Comps_ContentsMisc), reinterpret_cast<void*>(comp),
                   cn ? cn : "(no class)");
        }
    }

    bool FieldsOf(uintptr_t comp, std::vector<uintptr_t>& out)
    {
        out.clear();
        const uintptr_t map = comp + kOff_FieldMap;
        uint32_t n = 0;
        if (!CopyOut(map + kOff_Map_Count, &n, 4) || n > 100000) { g_why = "the field map does not read"; return false; }
        const uintptr_t entries = Deref(map + kOff_Map_Entries);
        if (n && !entries) { g_why = "the field map has no entries array"; return false; }
        for (uint32_t i = 0; i < n; ++i)
        {
            const uintptr_t node = Deref(entries + 8ull * i);
            if (!node) continue;
            const uintptr_t field = node + kOff_Node_Field;
            if (Deref(field) == Vt(kField)) out.push_back(field);
        }
        if (out.empty()) g_why = "the field map holds no FieldSaveData";
        return !out.empty();
    }

    bool Fields(std::vector<uintptr_t>& out)
    {
        out.clear();
        Candidate cands[8];
        const int n = ServerComponents(cands, 8);
        for (int i = 0; i < n; ++i)
        {
            if (!FieldsOf(cands[i].comp, out)) continue;
            if (cands[i].comp != g_lastComp && Say())
            {
                const char* an = gs::rtti::VtableClassName(reinterpret_cast<const void*>(cands[i].actorVt));
                GS_LOG("[savemap] the records are on list entry %d, a %s, one of %d component(s) in the list",
                       cands[i].entry, an ? an : "(no class)", n);
            }
            g_lastComp = cands[i].comp;
            return true;
        }
        return false;
    }

    // ---- what a loaded gimmick says about itself ----
    //
    // While a gimmick is loaded near the player, its saved record's state at
    // +0x21C reads zero, and it comes back when he walks off: a record on
    // AbyssRuins_Her_0027 went from Wait to zero at 1 metre and back to Wait
    // at 41. So a done standstone the player loaded in beside read as not
    // taken and got pinned. The live gimmick component holds the state at
    // +0x270 while it is loaded. Found by scanning every loaded gimmick's
    // component for the state hashes on 23 September: +0x270 held one on 162
    // of 164, and the operator on the finished Challenge_Standstone_Adventure_0015
    // read Complete there on four passes, 1 to 16 metres from the player.
    // +0x27C held the same value on most of them and is not used.
    constexpr uintptr_t kOff_LiveState = 0x270;

    // Placements a loaded gimmick says are done, joined the same way records
    // are. Only ever added to the taken set, never taken out of it.
    std::unordered_set<uintptr_t> g_liveSaid;
    int g_liveLogsLeft = 40, g_liveMissLogsLeft = 20;

    void LiveTaken(std::vector<int>& out)
    {
        out.clear();
        // On the heap: Entity's defaults are not all zero, so a static array of
        // them is stored in the plugin file, 575 KB of it.
        static std::vector<gs::actors::Entity> ents(4096);
        const int n = gs::actors::Snapshot(ents.data(), static_cast<int>(ents.size()));
        const gs::player::Pos pp = gs::player::Read();
        for (int i = 0; i < n; ++i)
        {
            const gs::actors::Entity& e = ents[i];
            if (!e.gimmick || !e.gimmickComp) continue;
            if (Deref(e.gimmickComp) != e.gimmickVt) continue;   // freed since it was found
            uint32_t st = 0;
            if (!CopyOut(e.gimmickComp + kOff_LiveState, &st, 4)) continue;
            // What the player carries is a gimmick too, standing wherever he
            // does. On 23 September his sword, always GimmickOn, stood 2.4
            // metres from a locked teleporter and made it read switched on.
            if (strncmp(e.name, "gimmick_equip_", 14) == 0) continue;
            // GimmickOn is done only when the gimmick is the teleporter
            // itself: its use-artifact gimmick went Wait to GimmickOn when
            // Seth switched one on, and its part stayed Wait.
            const bool ruinOn = st == kGimmickOn && strstr(e.name, "abyssruins_useartifact") != nullptr &&
                                strstr(e.name, "_part") == nullptr;
            if (!Taken(st) && !ruinOn) continue;
            const float p[3] = {e.x, e.y, e.z};
            float dist = 0;
            const int k = Nearest(p, &dist);
            if (ruinOn && !Taken(st) && (k < 0 || !Ruin(g_places[k].name))) continue;
            const float ex = e.x - pp.x, ez = e.z - pp.z;
            // Near the origin is the loading placeholder, not the player: it
            // printed 10,302 m for a ruin 9 m away on 23 September.
            const bool here = pp.valid && pp.x * pp.x + pp.z * pp.z >= 100.0f * 100.0f;
            const float away = here ? std::sqrt(ex * ex + ez * ez) : -1.0f;
            if (k < 0)
            {
                if (g_liveMissLogsLeft > 0 && g_liveSaid.insert(e.ptr).second && Say())
                {
                    --g_liveMissLogsLeft;
                    GS_LOG("[savemap] loaded \"%s\" eid %08X at (%.1f, %.1f, %.1f), %.0f m away, reads %s, and no "
                           "placement is within three metres of it", e.name[0] ? e.name : "unnamed", e.eid, e.x, e.y,
                           e.z, away, NameOf(st));
                }
                continue;
            }
            const gs::lgso::Place& q = g_places[k];
            out.push_back(k);
            if (g_liveLogsLeft > 0 && g_liveSaid.insert(q.at).second && Say())
            {
                --g_liveLogsLeft;
                GS_LOG("[savemap] loaded \"%s\" eid %08X, %.0f m away, reads %s, so record %u element %u \"%s\" "
                       "%.1f m from it is taken", e.name[0] ? e.name : "unnamed", e.eid, away, NameOf(st), q.record,
                       q.element, q.name[0] ? q.name : "unnamed", dist);
            }
        }
    }

    // ---- the game's own pick up message ----
    //
    // Picking up a sealed artifact moves no state the records or the live
    // gimmick carry, which a watch on both showed on 23 September: the item
    // stayed in Wait while the rocks mined beside it went to Break. The pick up
    // message is what does happen (game/pickup.cpp). Each id it names is looked
    // up in the entity set, joined to the nearest placement the same way a
    // record is, and that placement counts as taken from then on. The entity
    // set keeps an object twelve seconds after the game stops listing it, so
    // the item is still there to be found a second later.
    //
    // A placement whose name starts Challenge_ is handed to the pins file as
    // well, which writes it under the save being played once the game saves,
    // so the next load knows too. The
    // sealed artifacts and standstones are in that family and do not come
    // back. Veins, sockets and herbs do, so a pick up of one of those counts
    // for this session only.
    int g_pickLogsLeft = 60;

    void DrainPickups()
    {
        uint32_t ids[32];
        const int got = gs::pickup::Take(ids, 32);
        if (got == 0) return;
        static std::vector<gs::actors::Entity> ents(4096);
        const int n = gs::actors::Snapshot(ents.data(), static_cast<int>(ents.size()));
        for (int k = 0; k < got; ++k)
        {
            const gs::actors::Entity* e = nullptr;
            for (int i = 0; i < n && !e; ++i)
                if (ents[i].eid == ids[k]) e = &ents[i];
            if (!e)
            {
                if (g_pickLogsLeft > 0 && Say())
                {
                    --g_pickLogsLeft;
                    GS_LOG("[pickup] eid %08X is not in the entity set, so where it stood is not known", ids[k]);
                }
                continue;
            }
            const float p[3] = {e->x, e->y, e->z};
            float dist = 0;
            const int place = Nearest(p, &dist);
            if (place < 0)
            {
                if (g_pickLogsLeft > 0 && Say())
                {
                    --g_pickLogsLeft;
                    GS_LOG("[pickup] \"%s\" eid %08X at (%.1f, %.1f, %.1f) has no placement within three metres",
                           e->name[0] ? e->name : "unnamed", ids[k], e->x, e->y, e->z);
                }
                continue;
            }
            const gs::lgso::Place& q = g_places[place];
            const bool keep = strncmp(q.name, "Challenge_", 10) == 0;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                Take(place);
            }
            if (keep) gs::pinstore::AddTaken(q.x, q.y, q.z);
            if (g_pickLogsLeft > 0 && Say())
            {
                --g_pickLogsLeft;
                GS_LOG_OK("[pickup] \"%s\" eid %08X was picked up, so record %u element %u \"%s\" %.1f m from it "
                          "is taken%s", e->name[0] ? e->name : "unnamed", ids[k], q.record, q.element,
                          q.name[0] ? q.name : "unnamed", dist,
                          keep ? ", and kept for this save once the game saves" : " for this session");
            }
        }
    }

    // Live state changes around the player, for the teleporters. A saved
    // record reads zero while its gimmick is loaded, so switching one on shows
    // here first, if anywhere. Only gimmicks within 30 metres, and only a
    // change, named or as a hash. The first cut of this, on 23 September, also
    // tried to say when a gimmick stopped being listed and got it wrong; that
    // half is gone.
    std::unordered_map<uint32_t, uint32_t> g_liveLast;   // eid -> state last read
    int g_liveChangeLogsLeft = 200;

    void LiveChanges()
    {
        const gs::player::Pos pp = gs::player::Read();
        if (!pp.valid || pp.x * pp.x + pp.z * pp.z < 100.0f * 100.0f) return;
        static std::vector<gs::actors::Entity> ents(4096);
        const int n = gs::actors::Snapshot(ents.data(), static_cast<int>(ents.size()));
        std::vector<std::pair<uintptr_t, uint32_t>> byPlace;
        for (int i = 0; i < n; ++i)
        {
            const gs::actors::Entity& e = ents[i];
            if (!e.gimmick || !e.gimmickComp || !e.eid) continue;
            const float ex = e.x - pp.x, ez = e.z - pp.z;
            const float d = std::sqrt(ex * ex + ez * ez);
            if (d > 150.0f) continue;
            // An abyss gimmick is watched out to 150 metres, anything else to 30.
            const bool abyss = strstr(e.name, "abyss") != nullptr;
            if (!abyss && d > 30.0f) continue;
            if (Deref(e.gimmickComp) != e.gimmickVt) continue;
            uint32_t st = 0;
            if (!CopyOut(e.gimmickComp + kOff_LiveState, &st, 4)) continue;
            const float p[3] = {e.x, e.y, e.z};
            float pd = 0;
            const int place = Nearest(p, &pd);
            if (place >= 0) byPlace.push_back({g_places[place].at, st});
            const char* a = nullptr;
            auto it = g_liveLast.find(e.eid);
            if (it == g_liveLast.end())
            {
                g_liveLast.emplace(e.eid, st);
                if (abyss && g_liveChangeLogsLeft > 0 && Say())
                {
                    --g_liveChangeLogsLeft;
                    a = NameOf(st);
                    GS_LOG("[live] first sight of \"%s\" eid %08X at (%.1f, %.1f, %.1f), %.0f m away, reading %s%08X%s%s",
                           e.name, e.eid, e.x, e.y, e.z, d, a ? a : "", st, place >= 0 ? ", on placement " : "",
                           place >= 0 ? g_places[place].name : "");
                }
                continue;
            }
            if (it->second != st && g_liveChangeLogsLeft > 0 && Say())
            {
                --g_liveChangeLogsLeft;
                a = NameOf(it->second);
                const char* b = NameOf(st);
                GS_LOG("[live] \"%s\" eid %08X at (%.1f, %.1f, %.1f), %.0f m away, went from %s%08X to %s%08X%s%s",
                       e.name[0] ? e.name : "unnamed", e.eid, e.x, e.y, e.z, d, a ? a : "", it->second, b ? b : "",
                       st, place >= 0 ? ", on placement " : "", place >= 0 ? g_places[place].name : "");
            }
            it->second = st;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_liveByPlace.clear();
        for (const auto& kv : byPlace) g_liveByPlace[kv.first] = kv.second;
    }

    DWORD WINAPI Run(LPVOID)
    {
        size_t size = 0;
        if (!gs::typescan::ModuleRange(g_base, size)) return 0;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (!ResolveClasses()) return 0;

        // The component the survey was made from. A save loaded mid-session
        // gives the player a new one, and that save gets a survey of its own.
        uintptr_t surveyed = 0;
        bool missSaid = false;
        uint32_t missSince = 0, steadySince = 0;
        int lastCount = -1;
        std::vector<Rec> recs;
        std::vector<int> live;
        while (Pause(1000))
        {
            const uint32_t now = GetTickCount();
            if (!gs::player::Read().valid) continue;

            // The table fills in stages after a load; join only once it holds.
            const int c = gs::lgso::Count();
            if (c != lastCount) { lastCount = c; steadySince = now; }
            if (c < 5000 || now - steadySince < 3000) continue;
            if (g_places.empty() || gs::lgso::Generation() != g_placesGen) LoadPlaces();

            if (!Fields(g_fields))
            {
                if (!missSince) missSince = now;
                if (!missSaid && now - missSince > 20000)
                {
                    missSaid = true;
                    // What was taken belongs to the save it was read from. Kept
                    // through a load, it would hide glints in the next save that
                    // share nothing with it but a placement.
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_taken.clear();
                        g_takenAt.clear();
                    }
                    if (Say())
                    {
                        GS_LOG_ERR("[savemap] the save's gimmick records are out of reach (%s), so taken glints are "
                                   "not filtered", g_why);
                        DumpUsers();
                    }
                }
                continue;
            }
            if (missSaid && Say())
                GS_LOG_OK("[savemap] the save's gimmick records are in reach again, %u s after they were lost",
                          (now - missSince) / 1000);
            missSince = 0;
            missSaid = false;
            { gs::load::Timer t(gs::load::kSaveRead); ReadAll(recs); }
            if (surveyed != g_lastComp)
            {
                g_liveSaid.clear();   // a new save logs its own
                g_liveLast.clear();
            }
            {
                gs::load::Timer t(gs::load::kSaveLive);
                LiveTaken(live);
                DrainPickups();
                LiveChanges();
            }

            if (surveyed != g_lastComp)
            {
                if (surveyed && Say())
                    GS_LOG("[savemap] the player's server component is new (0x%p, was 0x%p), so a save was loaded; "
                           "reading its records afresh", reinterpret_cast<void*>(g_lastComp),
                           reinterpret_cast<void*>(surveyed));
                surveyed = g_lastComp;
                size_t total = 0;
                for (uintptr_t f : g_fields)
                {
                    uintptr_t b = 0;
                    size_t n = 0;
                    if (FieldRecords(f, &b, &n)) total += n;
                }
                if (Say())
                    GS_LOG_OK("[savemap] %zu field(s) in the save, %zu gimmick record(s), read straight from the "
                              "player's server component; %zu table placement(s) to join against", g_fields.size(),
                              total, g_places.size());
                Survey(recs);
                for (const Rec& r : recs) g_lastState[r.at] = r.state;
                { gs::load::Timer t(gs::load::kSavePublish); Publish(recs, live); }
                continue;
            }

            const gs::player::Pos pp = gs::player::Read();
            for (const Rec& r : recs)
            {
                auto it = g_lastState.find(r.at);
                const bool isNew = it == g_lastState.end();
                if (!isNew && it->second == r.state) continue;
                const float ex = r.pos[0] - pp.x, ez = r.pos[2] - pp.z;
                if (!r.hasPos || (pp.valid && ex * ex + ez * ez < 500.0f * 500.0f))
                    Line(isNew ? "new" : "changed", r, isNew ? 0 : it->second);
                g_lastState[r.at] = r.state;
            }
            { gs::load::Timer t(gs::load::kSavePublish); Publish(recs, live); }
        }
        return 0;
    }
}

namespace gs::savemap
{
    void Start()
    {
        bool expected = false;
        if (!g_started.compare_exchange_strong(expected, true)) return;
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_stopEvent) return;
        g_thread = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
        gs::load::AddThread("save reader", g_thread);
    }

    void Stop(bool processTerminating)
    {
        if (g_stopEvent) SetEvent(g_stopEvent);
        // On process teardown the loader lock is held and the thread is
        // already gone, so waiting on it is how an exit hangs.
        if (!processTerminating && g_thread) WaitForSingleObject(g_thread, 3000);
        if (g_thread) { CloseHandle(g_thread); g_thread = nullptr; }
    }

    bool Completed(uintptr_t placementAt)
    {
        if (!placementAt) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_taken.count(placementAt) != 0;
    }

    void Describe(uintptr_t placementAt, char* out, size_t n)
    {
        if (!out || !n) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_saveStates.find(placementAt);
        auto lv = g_liveByPlace.find(placementAt);
        char live[32] = "";
        if (lv != g_liveByPlace.end())
        {
            const char* nm = NameOf(lv->second);
            if (nm) _snprintf_s(live, sizeof(live), _TRUNCATE, ", live %s", nm);
            else _snprintf_s(live, sizeof(live), _TRUNCATE, ", live %08X", lv->second);
        }
        _snprintf_s(out, n, _TRUNCATE, "save %s%s%s", it == g_saveStates.end() ? "none" : it->second.c_str(), live,
                    g_taken.count(placementAt) ? ", TAKEN" : "");
    }

    bool TakenNear(float x, float y, float z)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Spot& s : g_takenAt)
        {
            const float ex = s.x - x, ez = s.z - z;
            if (std::fabs(s.y - y) <= kJoinHeight && ex * ex + ez * ez <= kJoinMetres * kJoinMetres) return true;
        }
        return false;
    }

    int TakenCount()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return static_cast<int>(g_taken.size());
    }

    uintptr_t Component() { return 0; }
}
