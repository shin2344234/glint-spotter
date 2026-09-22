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

#include "core/log.h"
#include "game/lgso.h"
#include "game/player.h"
#include "game/rtti.h"
#include "game/typescan.h"

namespace
{
    constexpr uintptr_t kRecordVtable = 0x058AFFC8;   // FieldGimmickSaveData
    constexpr uintptr_t kFieldVtable  = 0x058B0A48;   // FieldSaveData
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
    uintptr_t g_takenComp = 0;    // the component the set was read from
    uint32_t g_takenGen = 0;      // and the table generation
    uintptr_t g_lastComp = 0;     // the component Fields last reached

    uintptr_t g_base = 0;
    std::vector<gs::lgso::Place> g_places;
    std::vector<uintptr_t> g_fields;          // FieldSaveData objects
    std::unordered_map<uintptr_t, uint32_t> g_lastState;   // record address -> state last seen
    int g_linesLeft = 1500;

    bool Say()
    {
        if (g_linesLeft <= 0) return false;
        --g_linesLeft;
        return true;
    }

    bool CopyOut(uintptr_t at, void* out, size_t n)
    {
        if (at < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(at), n)) return false;
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
        if (vt != g_base + kRecordVtable) return false;
        r.at = at;
        r.hasPos = PosOf(b, kOff_OriginPos, r.pos) || PosOf(b, kOff_Pos, r.pos);
        memcpy(&r.state, b + kOff_State, 4);
        r.place = r.hasPos ? Nearest(r.pos, &r.dist) : -1;
        return true;
    }

    // ---- where the records are ----

    // A FieldSaveData's record vector: a pointer at +0x30 and a u32 count at
    // +0x38, which is how the load routine walks it (ServerContentsMisc slot
    // 10, 0x028B8CCF: rsi = [r12+0x30], end = rsi + [r12+0x38] * 0x3D8).
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
        if (Deref(ptr) != g_base + kRecordVtable ||
            Deref(ptr + (static_cast<size_t>(n) - 1) * kRecordBytes) != g_base + kRecordVtable)
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

    void Publish(const std::vector<Rec>& recs)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_takenComp != g_lastComp || g_takenGen != g_placesGen)
        {
            g_taken.clear();
            g_takenComp = g_lastComp;
            g_takenGen = g_placesGen;
        }
        for (const Rec& r : recs)
            if (Taken(r.state) && r.place >= 0) g_taken.insert(g_places[r.place].at);
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

    void Survey(const std::vector<Rec>& recs)
    {
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
    // ServerContentsMiscActorComponent (0x028B72F0). For each FieldSaveData
    // in the save it builds a copy and inserts it with 0x028EEC50 into a map
    // at this+0x2C0, keyed by the field. From the insert:
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
    constexpr uintptr_t kServerManagerGlobal = 0x06DA6798;
    constexpr uintptr_t kServerManagerVtable = 0x05B25B98;
    constexpr uintptr_t kServerUserVtable    = 0x05B255A0;
    constexpr uintptr_t kContentsMiscVtable  = 0x05B1D168;
    constexpr uintptr_t kOff_Mgr_User = 0xB8, kOff_Actor_Comps = 0x68, kOff_Comps_ContentsMisc = 0x368;
    constexpr uintptr_t kOff_FieldMap = 0x2C0, kOff_Map_Count = 0x04, kOff_Map_Entries = 0x18;
    constexpr uintptr_t kOff_Node_Field = 0x08;

    const char* g_why = "";

    uintptr_t ServerComponent()
    {
        const uintptr_t mgr = Deref(g_base + kServerManagerGlobal);
        if (!mgr || Deref(mgr) != g_base + kServerManagerVtable) { g_why = "no server actor manager"; return 0; }
        // +0xB8 points at a list, and the player is in it: the 15:30 wide read
        // logged "[ServerActorManager+0xB8]+0x0 holds it". The first entries
        // are checked rather than the first alone.
        const uintptr_t list = Deref(mgr + kOff_Mgr_User);
        uintptr_t user = 0;
        for (int i = 0; list && i < 8 && !user; ++i)
        {
            const uintptr_t a = Deref(list + 8ull * i);
            if (a && Deref(a) == g_base + kServerUserVtable) user = a;
        }
        if (!user) { g_why = "no player on the server"; return 0; }
        const uintptr_t comps = Deref(user + kOff_Actor_Comps);
        const uintptr_t comp = comps ? Deref(comps + kOff_Comps_ContentsMisc) : 0;
        if (!comp || Deref(comp) != g_base + kContentsMiscVtable) { g_why = "no ServerContentsMiscActorComponent"; return 0; }
        return comp;
    }

    bool Fields(std::vector<uintptr_t>& out)
    {
        out.clear();
        const uintptr_t comp = ServerComponent();
        if (!comp) return false;
        g_lastComp = comp;
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
            if (Deref(field) == g_base + kFieldVtable) out.push_back(field);
        }
        if (out.empty()) g_why = "the field map holds no FieldSaveData";
        return !out.empty();
    }

    DWORD WINAPI Run(LPVOID)
    {
        size_t size = 0;
        if (!gs::typescan::ModuleRange(g_base, size)) return 0;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        bool surveyed = false, missSaid = false;
        uint32_t missSince = 0, steadySince = 0;
        int lastCount = -1;
        std::vector<Rec> recs;
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
                if (!missSaid && now - missSince > 20000 && Say())
                {
                    missSaid = true;
                    GS_LOG_ERR("[savemap] the save's gimmick records are out of reach (%s), so taken glints are not "
                               "filtered", g_why);
                }
                continue;
            }
            missSince = 0;
            ReadAll(recs);

            if (!surveyed)
            {
                surveyed = true;
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
                Publish(recs);
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
            Publish(recs);
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

    int TakenCount()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return static_cast<int>(g_taken.size());
    }

    uintptr_t Component() { return 0; }
}
