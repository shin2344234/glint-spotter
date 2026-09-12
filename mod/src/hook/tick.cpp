#include "hook/tick.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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
#include "game/lgso.h"
#include "game/camera.h"
#include "game/dump.h"
#include "game/physics.h"
#include "game/pinmodel.h"
#include "game/realpin.h"
#include "core/pinstore.h"
#include "game/saveslot.h"
#include "hook/pad.h"
#include "core/settings.h"

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
        gs::camera::LogSample(g_samples);
        {
            static uint64_t lastCalls = 0, lastTicks = 0;
            const uint64_t calls = gs::camera::Calls(), ticks = g_count.load();
            if (lastTicks && ticks > lastTicks)
                GS_LOG("[cam %llu] update ran %.2f times per tick, this 0x%p", static_cast<unsigned long long>(g_samples),
                       static_cast<double>(calls - lastCalls) / static_cast<double>(ticks - lastTicks),
                       reinterpret_cast<void*>(gs::camera::This()));
            lastCalls = calls; lastTicks = ticks;
        }

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
    uint64_t g_lastRefreshTick = 0;

    // The world map root control does not exist until the map is opened
    // once. Session twenty-two placed a pin on what the sweep offered
    // instead, a registry entry, and the game went down. Pins asked for
    // before the spy has seen the real root wait here.
    struct Pending { float x, y, z; char label[16]; int64_t id; bool haveId; };
    constexpr int kPendingMax = 32;
    Pending g_pending[kPendingMax];
    int g_pendingN = 0;

    bool PendingNear(float x, float z, float radius)
    {
        for (int i = 0; i < g_pendingN; ++i)
        {
            const float dx = g_pending[i].x - x, dz = g_pending[i].z - z;
            if (dx * dx + dz * dz <= radius * radius) return true;
        }
        return false;
    }

    // What this flash has already marked.
    //
    // The automatic marker used to have no memory at all. The only thing
    // stopping it marking the same glint twice was a pin sitting on the map
    // near that place, so deleting the pin erased the memory and the still lit
    // glint was new again: back on within a second, and if the target resolved
    // a few metres differently the second time, back on in the wrong place.
    //
    // Two wrong fixes came before this one. A list of places a pin had been
    // taken off, which was a stop bolted to the outside of the thing that was
    // wrong inside and needed a timer to stop being permanent. Then a list of
    // things marked, kept for the whole world, which stopped a glint being
    // marked ever again once its pin was deleted. Both got the scope wrong.
    //
    // The right scope is one press of the flash. Inside a single flash, each
    // thing is marked once, so deleting a pin while the flash is still lit
    // does not fetch it straight back. Press the flash again and everything is
    // eligible again, because pressing it again is somebody asking. Nothing
    // here expires on a clock and nothing keys on a place alone.
    //
    // A candidate carries an identity: nodes have the game's entity id, level
    // table placements have a record and an element number. The position comes
    // along as well, because the same object can arrive by either route and
    // the two identities cannot be matched to each other.
    // Three kinds of thing end up in here, and only two of a kind can be
    // compared. An entity id means nothing to a level table record, so where
    // they stand is all those two have in common, and a place on its own is
    // what a deleted pin leaves behind.
    enum class MarkKind { Node, Table, Place };

    struct MarkedThing
    {
        MarkKind kind = MarkKind::Place;
        uint32_t eid = 0;       // Node
        uint32_t record = 0;    // Table
        uint32_t element = 0;
        float x = 0.0f, z = 0.0f;
    };
    // How close two pins may be before they count as the same place. Used by
    // the dedupe below as well, which is why it lives up here.
    constexpr float kPinApart = 4.0f;
    // No cap. A cap has to answer what happens when it fills, and both
    // answers are wrong: forgetting the oldest lets something marked early in
    // the flash be marked again once its pin is deleted, and refusing the
    // newest does the same to the thing just marked. The list is cleared by
    // every press and holds twenty bytes an entry, and clear() keeps the
    // memory it has already taken, so after the first flash it stops
    // allocating at all.
    std::vector<MarkedThing> g_marked;

    bool AlreadyMarked(MarkKind kind, uint32_t eid, uint32_t record, uint32_t element,
                       float x, float z)
    {
        for (const MarkedThing& m : g_marked)
        {
            // Two of a kind answer for themselves. Two entities three metres
            // apart are two entities, and a position test between them would
            // silence one for no reason.
            if (kind == MarkKind::Node && m.kind == MarkKind::Node)
            {
                if (m.eid == eid) return true;
                continue;
            }
            if (kind == MarkKind::Table && m.kind == MarkKind::Table)
            {
                if (m.record == record && m.element == element) return true;
                continue;
            }
            const float dx = m.x - x, dz = m.z - z;
            if (dx * dx + dz * dz <= kPinApart * kPinApart) return true;
        }
        return false;
    }

    void RememberMarked(MarkKind kind, uint32_t eid, uint32_t record, uint32_t element,
                        float x, float z)
    {
        MarkedThing m;
        m.kind = kind;
        m.eid = eid;
        m.record = record;
        m.element = element;
        m.x = x;
        m.z = z;
        g_marked.push_back(m);
    }

    // A pin taken off the map is settled for this press too. Without it the
    // one case that started all of this comes back: mark a glint, let the
    // flash end, press it again, and delete the old pin while that second
    // press is still lit. Nothing had marked the glint during this press, and
    // the pin that was suppressing it has just gone, so it went straight back
    // on. The next press clears this like everything else, so the glint can be
    // marked again by asking again.
    void SettleAt(float x, float z)
    {
        if (AlreadyMarked(MarkKind::Place, 0, 0, 0, x, z)) return;
        RememberMarked(MarkKind::Place, 0, 0, 0, x, z);
    }

    void ForgetMarked() { g_marked.clear(); }

    // A record the map's own user interface can see, and an icon keyed on it.
    // Everything that puts a pin on the map goes through here.
    // False when nothing went on the map. A root the map has since thrown
    // away still reads as a pointer, and PlacePinNow catches that and places
    // nothing; discarding its answer here is what let the automatic marker
    // write down a mark it never made.
    bool PlacePin(void* root, float x, float y, float z, const char* label)
    {
        int64_t realId = 0;
        bool haveReal = false;
        if (gs::Settings::Get().realMarkers) haveReal = gs::realpin::Place(x, z, &realId);
        if (gs::mapicon::PlacePinNow(root, x, y, z, label, realId, haveReal)) return true;
        // The record went into the marker list before the icon was asked for,
        // so a refusal here leaves one with nothing drawn on it. The pin stays
        // queued and the next attempt mints another id, so this one has to go
        // or the list fills with records no icon points at.
        if (haveReal)
        {
            gs::realpin::Retire(realId);
            GS_LOG("[mark] nothing drew, so the record made for it is taken back out");
        }
        return false;
    }

    void FlushPending()
    {
        if (g_pendingN == 0) return;
        void* root = gs::mapicon::LastWorldRoot();
        if (!root) return;
        GS_LOG("[mark] the world map root exists now; placing %d queued pin(s)", g_pendingN);
        // What will not place stays queued. The queue used to be emptied
        // whatever happened, so a root the map had thrown away between the
        // queueing and the flush lost those pins with nothing left to say so.
        int kept = 0;
        for (int i = 0; i < g_pendingN; ++i)
        {
            if (PlacePin(root, g_pending[i].x, g_pending[i].y, g_pending[i].z, g_pending[i].label))
                continue;
            g_pending[kept++] = g_pending[i];
        }
        if (kept)
            GS_LOG_ERR("[mark] %d queued pin(s) would not go on the map and are still waiting",
                       kept);
        g_pendingN = kept;
    }

    bool QueuePin(float x, float y, float z, const char* label)
    {
        if (g_pendingN >= kPendingMax) return false;
        Pending& p = g_pending[g_pendingN++];
        p.x = x; p.y = y; p.z = z;
        strncpy_s(p.label, sizeof(p.label), label, _TRUNCATE);
        return true;
    }

    // A load builds a new player, and the marker copy the mod writes hangs off
    // it, so the pointer changing is the mod's cue that a world has appeared.
    // Everything it drew before that belongs to a map that no longer exists.
    std::atomic<bool> g_worldRebuilt{false};

    // The save the world about to appear will be, held rather than acted on
    // until it does appear. Two of them: the one whose file was read through,
    // which is a load, and the one merely opened, which is the game taking a
    // look and only counts when nothing was read through.
    gs::saveslot::Id g_pendingFull;
    gs::saveslot::Id g_pendingPeek;
    bool g_everSawLoad = false;

    // What the file watch caught. A write is unambiguous, since the game only
    // writes the save being played, so that one is acted on where it lands.
    void PumpSaveEvents()
    {
        gs::saveslot::Event ev[16];
        for (int round = 0; round < 8; ++round)
        {
            const int n = gs::saveslot::Take(ev, 16);
            for (int i = 0; i < n; ++i)
            {
                char text[64]{};
                gs::saveslot::Text(ev[i].id, text, sizeof(text));
                if (ev[i].write)
                {
                    GS_LOG("[save] the game wrote %s", text);
                    gs::pinstore::SavedTo(ev[i].id.account, ev[i].id.slot);
                }
                else if (ev[i].full)
                {
                    GS_LOG_OK("[save] the game read the whole of %s, so it is loading it", text);
                    g_pendingFull = ev[i].id;
                    g_everSawLoad = true;
                }
                else
                {
                    GS_LOG("[save] the game opened %s", text);
                    g_pendingPeek = ev[i].id;
                }
            }
            if (n < 16) break;
        }
    }

    void RestoreOnNewWorld()
    {
        static uintptr_t seen = 0;
        static uint32_t lastMs = 0;
        static bool pending = false;

        // A player who answers with a position is a player the mod can write
        // through. Without this the restore can put records into a component
        // the load has already thrown away.
        if (!gs::player::Read().valid) return;
        const uintptr_t sub = gs::pinmodel::Submodule();
        if (!sub) return;

        // A load builds a new player, so the actor moving is a world being
        // built, and unlike the map's icons it moves once. The first signal
        // tried here was the game creating a MapIcon_ActorFocus, which is the
        // player's own arrow and is remade several times a second.
        static uintptr_t seenActor = 0;
        const uintptr_t actor = gs::player::Actor();
        if (actor && actor != seenActor)
        {
            if (seenActor)
            {
                pending = true;
                g_worldRebuilt.store(true);
                GS_LOG("[pins] the player has been rebuilt, so a world was loaded; the pins go "
                       "back on");
            }
            seenActor = actor;
        }

        // Two ways to know a world has been built. The marker copy sitting at
        // an address the mod has not seen is the obvious one, and it catches a
        // fresh launch because the process starts over. The other is the
        // records going missing underneath pins that are still on the map,
        // which is what loading a save without leaving the game does: session
        // a hundred and fourteen did exactly that, the map threw away three
        // hundred icons and built seven hundred and fifty, and the marker copy
        // came back at the same address, so the first test saw nothing.
        const bool fresh = sub != seen;
        int live = 0, mine = 0;
        bool lost = false;
        if (!fresh && gs::Settings::Get().realMarkers)
        {
            live = gs::mapicon::LivePinsAtOrAbove(gs::realpin::IdBase());
            mine = gs::realpin::MineInList();
            lost = live > 0 && mine >= 0 && mine < live;
        }
        if (!fresh && !lost && !pending) return;

        // Twenty seconds between restores, so that whatever the cause, one bad
        // reading cannot put a second copy of every pin on the map.
        const uint32_t now = GetTickCount();
        if (lastMs && now - lastMs < 20000) return;
        lastMs = now;
        seen = sub;
        pending = false;

        // Now the world is here, the read in front of it was a load, and the
        // pins that come back below are that save's. A world with no read in
        // front of it is a new game instead, which is only trusted once the
        // watch has proved it can see a load at all: a watch that sees nothing
        // would call every world a new game and hand each one an empty set.
        const gs::saveslot::Id chosen = g_pendingFull.ok() ? g_pendingFull : g_pendingPeek;
        if (chosen.ok())
        {
            char text[64]{};
            gs::saveslot::Text(chosen, text, sizeof(text));
            GS_LOG_OK("[save] the world is %s%s", text,
                      g_pendingFull.ok() ? "" : ", going by which file was opened last, since "
                                                "none was read through");
            gs::pinstore::Loaded(chosen.account, chosen.slot);
            g_pendingFull = gs::saveslot::Id{};
            g_pendingPeek = gs::saveslot::Id{};
        }
        else if (g_everSawLoad)
        {
            gs::pinstore::NewGame();
        }

        if (lost)
            GS_LOG("[pins] %d pin(s) on the map and %d record(s) left behind them, so the world "
                   "has been rebuilt", live, mine);

        // Take the old ones off before putting them back. On a real rebuild
        // the icons and records are gone already and this does nothing, and
        // any other time it is what keeps a second restore from leaving two
        // pins on every place.
        void* root = gs::mapicon::LastWorldRoot();
        int64_t old[256];
        const int oldN = gs::mapicon::LivePinKeys(old, 256);
        for (int i = 0; i < oldN; ++i)
        {
            if (old[i] >= gs::realpin::IdBase()) gs::realpin::Retire(old[i]);
            gs::mapicon::RemoveIcon(root, old[i]);
        }
        if (oldN) GS_LOG("[pins] %d old pin(s) taken off first", oldN);
        // Entity ids belong to the world that issued them.
        ForgetMarked();
        gs::mapicon::ForgetAll();
        g_pendingN = 0;
        if (!gs::Settings::Get().keepPins) return;
        gs::pinstore::Saved saved[256];
        const int n = gs::pinstore::All(saved, 256);
        if (n == 0) return;
        for (int i = 0; i < n; ++i) QueuePin(saved[i].x, saved[i].y, saved[i].z, saved[i].label);
        GS_LOG_OK("[pins] %s; %d pin(s) from the file are queued and go on as soon as the map has "
                  "been opened once", fresh ? "a world" : "the world was rebuilt", n);
    }

    // True when a pin was placed or queued, false when nothing came of it. The
    // automatic marker writes down what it has marked and must not write down
    // what it failed to place.
    bool PlaceAt(float tx, float ty, float tz, const char* how, const char* label, const gs::player::Pos& pp, float dedupe)
    {
        if (gs::mapicon::PinNear(tx, tz, dedupe) || PendingNear(tx, tz, dedupe))
        {
            GS_LOG("[mark] a pin already sits within %.0f units of (%.1f, %.1f); not placing another", dedupe, tx, tz);
            return false;
        }
        const float dx = tx - pp.x, dz = tz - pp.z;
        GS_LOG("[mark] target %.1f units away via %s; placing a %s pin at (%.1f, %.1f, %.1f)",
               std::sqrt(dx * dx + dz * dz), how, label, tx, ty, tz);

        // The game's own marker first, because that is the one I can
        // delete. The record goes into the player's marker list and comes back
        // with an id, and that id is what the icon below is keyed on. The game
        // did not draw the record itself in session a hundred and three, so
        // the mod still draws it; what changed is that the thing on the map
        // now names a marker the game knows about instead of a number the mod
        // made up. A zero here means no record, and the pin is a picture again.

        // Only a root the spy has seen the game call slot 170 on. The
        // sweep's candidate is never used for a call.
        void* root = gs::mapicon::LastWorldRoot();
        if (!root)
        {
            if (g_pendingN < kPendingMax)
            {
                Pending& p = g_pending[g_pendingN++];
                p.x = tx; p.y = ty; p.z = tz;
                strncpy_s(p.label, sizeof(p.label), label, _TRUNCATE);
                gs::pinstore::Add(tx, ty, tz, label);
                GS_LOG("[mark] the world map has not been opened this session, so its root does not exist yet; "
                       "pin queued (%d waiting). Open the map once and it appears.", g_pendingN);
                return true;
            }
            GS_LOG_ERR("[mark] %d pins already waiting for the map to be opened; this one is dropped", g_pendingN);
            return false;
        }
        const bool onTheMap = PlacePin(root, tx, ty, tz, label);
        // Written down either way. The record and the id exist by now, and a
        // pin in the file is one the restore can put back; a pin that never
        // drew is not one to forget about.
        gs::pinstore::Add(tx, ty, tz, label);
        return onTheMap;
    }

    // The calibration is finished, and it passed. Session fifty-four put "Me"
    // on the player, "N200" two hundred metres north of him and "E100" a
    // hundred east, and the screenshot of the map shows exactly that
    // geometry: the two offset pins sit north and east of my arrow at the
    // right distances, and the session's own glint pin sits just north of me
    // where its node stood, twenty-four metres away. A pin goes where it is
    // asked to. Nothing about the coordinates, the frame, the height or the
    // call needs looking at again.
    //
    // Which leaves the pick, and the pick cannot be judged from a log alone
    // because only a player can see which object is glowing. So the three pins
    // that proved the coordinates go to the candidates instead: once per
    // place, every node inside the cone gets a pin labelled with how far away
    // it is, and the log lists the same nodes by name. He reads one number off
    // the map and the mod knows what the glint is called.
    bool g_surveyed = false;
    float g_surveyX = 0, g_surveyZ = 0;

    bool FarFrom(float x, float z, float fromX, float fromZ, float metres)
    {
        const float dx = x - fromX, dz = z - fromZ;
        return std::sqrt(dx * dx + dz * dz) > metres;
    }

    void Survey(const gs::player::Pos& pp, const gs::actors::Entity* around, const float* angles, int n)
    {
        if (n <= 0) return;
        if (!gs::Settings::Get().survey) return;
        // Fifty metres, so walking toward a glint re-arms it. The point of
        // the walk is to stand on the thing and have the mod name it.
        if (g_surveyed && !FarFrom(pp.x, pp.z, g_surveyX, g_surveyZ, 50.0f)) return;
        g_surveyed = true;
        g_surveyX = pp.x;
        g_surveyZ = pp.z;
        GS_LOG("[survey] every node inside the cone gets a pin labelled with its distance. "
               "Open the map, find the one sitting on the glint, and its number names it below.");
        for (int i = 0; i < n && i < 5; ++i)
        {
            const float dx = around[i].x - pp.x, dz = around[i].z - pp.z;
            const float d = std::sqrt(dx * dx + dz * dz);
            char label[16];
            _snprintf_s(label, sizeof(label), _TRUNCATE, "%.0fm", d);
            GS_LOG("[survey]   \"%s\" is \"%s\" eid %08X, %.1f degrees off the crosshair",
                   label, around[i].name[0] ? around[i].name : "?", around[i].eid, angles[i] * 57.2958f);
            PlaceAt(around[i].x, around[i].y, around[i].z, "survey", label, pp, 0.0f);
        }
    }

    // The view ray. The camera's own forward when its object is in hand,
    // otherwise the body's facing held level. The origin is eye height.
    struct View
    {
        float ox = 0, oy = 0, oz = 0;
        float fx = 0, fy = 0, fz = 1;
        bool camera = false;
    };

    int g_sayOriginLeft = 6;

    bool ViewRay(const gs::player::Pos& pp, View* v)
    {
        v->ox = pp.x; v->oy = pp.y + 1.6f; v->oz = pp.z;
        const gs::camera::Pose cam = gs::camera::Read();
        if (cam.fwdValid)
        {
            v->fx = cam.fwd[0]; v->fy = cam.fwd[1]; v->fz = cam.fwd[2];
            v->camera = true;
            // The crosshair ray leaves the camera, which sits `dist` behind
            // the pivot and a little above it, not the player's eye. At a
            // grazing pitch that height is tens of units on the ground.
            // The pivot is in the sub-level's frame; the player's world
            // minus local offset moves it to the map's.
            if (cam.dist > 0.5f && cam.dist < 30.0f)
            {
                const float cx = cam.pivot[0] + (pp.x - pp.lx) - cam.fwd[0] * cam.dist;
                const float cy = cam.pivot[1] + (pp.y - pp.ly) - cam.fwd[1] * cam.dist + 0.6f;
                const float cz = cam.pivot[2] + (pp.z - pp.lz) - cam.fwd[2] * cam.dist;
                // The pivot is in the sub-level's frame and the player's world
                // minus local offset moves it to the map's, which is right only
                // while both are in the same frame. When the answer lands far
                // from the player it is not the camera, so the eye is used.
                const float dx = cx - pp.x, dy = cy - pp.y, dz = cz - pp.z;
                if (dx * dx + dy * dy + dz * dz <= 40.0f * 40.0f)
                {
                    v->ox = cx; v->oy = cy; v->oz = cz;
                }
                else if (g_sayOriginLeft > 0)
                {
                    --g_sayOriginLeft;
                    GS_LOG("[auto] the camera origin came out at (%.1f, %.1f, %.1f), %.0f from the player at (%.1f, %.1f, %.1f); using the eye",
                           cx, cy, cz, std::sqrt(dx * dx + dy * dy + dz * dz), pp.x, pp.y, pp.z);
                }
            }
            return true;
        }
        float fx = 0, fz = 0;
        if (!gs::nearest::ForwardFromQuat(pp.q, &fx, &fz)) return false;
        v->fx = fx; v->fy = 0; v->fz = fz;
        v->camera = false;
        return true;
    }

    // Where the view ray meets the plane of the player's feet. Only with a
    // camera pitch, and only looking down.
    bool GroundPoint(const View& v, float* gx, float* gy, float* gz, float* t)
    {
        if (!v.camera || v.fy > -0.03f) return false;
        const float tt = 1.6f / (-v.fy);
        if (tt > 120.0f) return false;
        *t = tt;
        *gx = v.ox + v.fx * tt; *gy = v.oy + v.fy * tt; *gz = v.oz + v.fz * tt;
        return true;
    }

    int CastView(const View& v, float maxAlong, float radius, float spread, bool glintOnly,
                 gs::nearest::Candidate* c, int n, gs::nearest::Candidate* miss, int missN,
                 float nearAll = 0.0f)
    {
        if (v.camera)
            return gs::nearest::Cast3D(gs::player::Actor(), v.ox, v.oy, v.oz, v.fx, v.fy, v.fz,
                                       maxAlong, radius, spread, glintOnly, c, n, miss, missN, nearAll);
        return gs::nearest::Cast(gs::player::Actor(), v.ox, v.oy, v.oz, v.fx, v.fz,
                                 maxAlong, radius, spread, glintOnly, c, n, miss, missN);
    }

    // The classes in an entity's component block, one line. Session
    // twenty-four's aimed object was a ClientNormalInGameActor with no
    // gimmick component; this says what it carries instead.
    void DescribeComponents(const char* tag, uintptr_t entity)
    {
        char line[900];
        int w = 0;
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(entity + 0x68), 8)) return;
            const uintptr_t comps = *reinterpret_cast<const uintptr_t*>(entity + 0x68);
            if (comps < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(comps), 0x80)) return;
            for (uintptr_t off = 0; off < 0x80 && w < 800; off += 8)
            {
                const uintptr_t c = *reinterpret_cast<const uintptr_t*>(comps + off);
                if (c < 0x10000 || (c & 7) != 0 || !gs::rtti::Readable(reinterpret_cast<const void*>(c), 8)) continue;
                const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(c);
                const char* n = gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt));
                if (!n) continue;
                if (n[0] == '.') n += 4;
                const char* end = strstr(n, "@");
                const int len = end ? static_cast<int>(end - n) : static_cast<int>(strlen(n));
                const int k = _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " +%02llX:%.*s", static_cast<unsigned long long>(off), len, n);
                if (k < 0) break;
                w += k;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        line[w] = 0;
        GS_LOG("[mark]     %s components:%s", tag, line);
    }

    // Flash on: find out which node the flash is lighting. Pin nothing until
    // that is known.
    //
    // My rule, and it is the right one: only the glint gets a pin. The mod
    // cannot yet tell which node is glinting. Two fields named from
    // disassembly both flipped with the flash off, and marking every
    // collectible nearby instead was a guess wearing the feature's clothes,
    // so it is gone.
    //
    // What is left is the measurement that settles it. While the flash is on,
    // the four nearest nodes on the mark list have their gimmick component
    // and their effect component's activity recorded; two seconds after the
    // flash ends, the same four again. The node I see glowing is one of
    // them, and the field that means revealed is the one that moved for that
    // node and stayed put on the other three.
    uint32_t g_flashOnMs = 0;
    bool g_flashWas = false;
    bool g_probedThisPress = false;
    int g_flashProbesLeft = 3;
    int g_setListingsLeft = 6;
    bool g_setListedOnce = false;
    float g_setListedX = 0, g_setListedZ = 0;
    int g_targetLogsLeft = 12;
    uint32_t g_targetLastMs = 0;
    int g_glintWinsLeft = 40;
    int g_tableLogsLeft = 120;
    int g_quietLogsLeft = 20;
    // How close two automatic pins may be. Eight metres was arbitrary and
    // it is wide enough to swallow a neighbour: glints come in clusters and
    // pinning one should not refuse the next one along. Four.
    int g_dupLogsLeft = 20;
    // Its own small budget rather than the duplicate message's, which is the
    // more useful of the two and should not be spent by this one.
    int g_markedLogsLeft = 4;
    uint32_t g_markedLastMs = 0;
    int g_pinModelLogsLeft = 6;
    int g_losLogsLeft = 20;
    uint32_t g_losLastMs = 0;
    uint32_t g_dupLastMs = 0;
    uint32_t g_quietLastMs = 0;
    int g_lastGlintN = -1;
    int g_heldWinsLeft = 40;   // how many times the log says the target took the pick
    int g_loadingLogsLeft = 4;
    uint32_t g_heldEid = 0;
    uint32_t g_heldSinceMs = 0;
    float g_heldX = 0, g_heldZ = 0;
    uint32_t g_cooldownUntil = 0;
    int g_autoLogsLeft = 60;
    uint32_t g_autoLastLogMs = 0;
    uintptr_t g_huntEnts[4] = {0, 0, 0, 0};
    uint32_t g_huntEids[4] = {0, 0, 0, 0};
    char g_huntNames[4][48]{};
    int g_huntOn[4] = {0, 0, 0, 0};
    int g_huntN = 0;
    // Six rounds of a four entity, eleven hundred byte dump, on the frame
    // thread, every time the flash is held. It answered its question long
    // ago and it is most of the log, so it waits for Verbose now.
    int g_huntLeft = 0;
    uint32_t g_huntLastMs = 0;
    uint32_t g_huntOffMs = 0;

    void AutoMark(uint32_t now)
    {
        static bool huntArmed = false;
        if (!huntArmed)
        {
            huntArmed = true;
            if (gs::Settings::Get().verbose) g_huntLeft = 6;
        }
        const bool flash = gs::aim::FlashActive();
        if (!flash)
        {
            g_flashWas = false;
            if (g_huntN)
            {
                if (!g_huntOffMs) g_huntOffMs = now;
                else if (now - g_huntOffMs > 2000)
                {
                    for (int i = 0; i < g_huntN; ++i)
                    {
                        const int off = gs::dump::EffectActivity(g_huntEnts[i], 0x400);
                        GS_LOG("[hunt] \"%s\" eid %08X: effects %d with the flash on, %d with it off%s",
                               g_huntNames[i], g_huntEids[i], g_huntOn[i], off,
                               (off >= 0 && g_huntOn[i] >= 0 && off != g_huntOn[i]) ? "  <- this one changed" : "");
                        char tag[24];
                        _snprintf_s(tag, sizeof(tag), _TRUNCATE, "off%d", i + 1);
                        gs::dump::GimmickState(tag, g_huntEnts[i], 0x480);
                    }
                    g_huntN = 0;
                    g_huntOffMs = 0;
                }
            }
            return;
        }
        g_huntOffMs = 0;
        if (!g_flashWas)
        {
            g_flashWas = true;
            g_flashOnMs = now;
            g_probedThisPress = false;
            // A new flash is a new ask. Whatever the last one marked stops
            // counting here, so a glint whose pin was deleted can be marked
            // again by pressing again.
            ForgetMarked();
        }

        // What the game itself decided the flash lit. The detect component
        // keeps a FindDetectTargetTask at +0x1D0, named by RTTI and never once
        // read in fifty sessions, and the special mode component fills three
        // pointer fields the moment the flash fires. The mod is still guessing
        // which node is glinting from a bearing; one of these objects knows.
        // Half a second after the press, so the task has run, three presses a
        // session, read only.
        if (!g_probedThisPress && g_flashProbesLeft > 0 && now - g_flashOnMs > 500)
        {
            g_probedThisPress = true;
            --g_flashProbesLeft;
            const uintptr_t task = gs::aim::DetectTask();
            if (task)
            {
                GS_LOG("[flash] FindDetectTargetTask at 0x%p", reinterpret_cast<void*>(task));
                // Out to 0x600: session nineteen's probe found the detected
                // actor at +0x540 and session fifty-two's dump stopped at
                // 0x200, which is why the log showed an empty task.
                gs::dump::Pointers("task", task, 0x600);
                gs::dump::Object("taskhex", task, 0x600);
            }
            else
            {
                GS_LOG("[flash] no FindDetectTargetTask on the detect component");
            }
            if (const uintptr_t special = gs::aim::SpecialComponent())
            {
                gs::dump::Pointers("special", special, 0x600);
                gs::dump::Object("specialhex", special, 0x600);
            }
            if (const uintptr_t detect = gs::aim::DetectComponent())
            {
                gs::dump::Pointers("detectp", detect, 0x700);
                gs::dump::Object("detecthex", detect, 0x700);
            }
        }
        const gs::player::Pos pp = gs::player::Read();
        if (!pp.valid) return;

        // Not while the world is loading. Session fifty-three teleported
        // between two glints and the player read (0.0, 0.6, -1.6) on the way
        // through, with the entity set still holding the place he had left:
        // "the farthest marked node is 11524 metres out". Two pins went onto
        // the map at (15.2, 1.0, -5.6) and (2.4, 1.9, -5.7), which is nowhere.
        // Real play coordinates in this game run to thousands of units, so a
        // player within a hundred of the absolute origin is a placeholder.
        if (std::fabs(pp.x) + std::fabs(pp.z) < 100.0f)
        {
            if (g_loadingLogsLeft > 0)
            {
                --g_loadingLogsLeft;
                GS_LOG("[auto] the player reads (%.1f, %.1f, %.1f), which is the placeholder the game "
                       "uses while a world loads; nothing is pinned until he is somewhere",
                       pp.x, pp.y, pp.z);
            }
            g_heldSinceMs = 0;
            return;
        }

        // The node the crosshair is on: of every marked node the game has
        // loaded, the one whose bearing from the camera is nearest the view's,
        // within fifteen degrees, and at least three metres from the player so
        // that something underfoot cannot take it. Held for a second, it gets
        // one pin at its own position and nothing else does.
        //
        // Every marked node, not a nearest handful. Session fifty asked for
        // the sixteen nearest and then chose among those, and the log says
        // what that cost: twenty-seven marked nodes loaded, sixteen in reach,
        // and the pin went to a berry bush twenty-two metres out. The eleven
        // it dropped were the eleven farthest, which is where the glint was.
        // A bearing test wants the far ones most: a bush three metres away
        // sitting half a metre off the line is nine degrees wide, while a
        // glint a hundred metres out has to be within a metre to read as one.
        //
        // Heights are not in this: session forty had firewood a metre away
        // reading six metres below the player's feet, so an angle measured in
        // three axes is unusable. A bearing is not.
        // The gimmick's own detect mode target byte, when anything in the set
        // carries it. This is the thing the project has been looking for: a
        // flag the game sets on the object that glints, rather than a guess
        // from where the crosshair points. It never moved in fifty-six
        // sessions because the object carrying it is not in the pools until
        // the player is near it, and session fifty-seven caught it the moment
        // Walking toward the glint I had marked:
        //
        //   [14:59:22.881] glint byte set on eid B0100301 at (-9706.3, 566.7, -4162.5)
        //
        // Nearest the crosshair wins among them, in a wider cone than the
        // bearing pick gets, because this is the game saying so rather than
        // the mod inferring it.
        gs::actors::Entity glints[8];
        float glintAngles[8];
        int glintN = 0;

        // The level gimmick table, which is where the answer lives.
        //
        // Session sixty-seven settled it. With the player at (-9706.5, 559.9,
        // -4235.0) the table put a placement at (-9715.7, 567.5, -4142.7), and
        // My own marker on the glint I have been testing against since
        // session forty-seven sits at (-9714.073, -4141.196). Two metres apart.
        // The thing I aim at is in the table, and the table does not care how
        // far away I am: 17,728 placements across the whole map, read from a
        // fixed global.
        //
        // Everything before this picked from what the actor manager had
        // streamed, which never reached past about sixty-six metres, so a
        // glint a hundred and nineteen metres out could not be chosen no
        // matter how good the aim was.
        gs::lgso::Place table[8];
        float tableAngles[8];
        int tableN = 0;

        gs::actors::Entity around[8];
        float angles[8];
        const float cap = gs::Settings::Get().radius;
        int n = 0;
        int marked = 0;
        gs::aim::Held held;
        View v;
        if (ViewRay(pp, &v))
        {
            const float flen = std::sqrt(v.fx * v.fx + v.fz * v.fz);
            if (flen > 1e-3f)
            {
                n = gs::actors::MarkedOnBearing(pp.x, pp.z, v.ox, v.oz, v.fx / flen, v.fz / flen,
                                                cap > 0.0f ? cap : 1.0e9f, 0.26f, 3.0f,
                                                around, angles, 8, &marked);
                glintN = gs::actors::GlintOnBearing(pp.x, pp.z, v.ox, v.oz, v.fx / flen, v.fz / flen,
                                                    glints, glintAngles, 8);
                // Eight metres off the line or three per cent of the range,
                // whichever is more. Four flat was too tight: I tested from
                // the air, where the crosshair sways, and a placement four
                // hundred metres out left the cone between passes so the
                // second a pin needs never came. Three per cent is twelve
                // metres at four hundred and twenty-seven at nine hundred,
                // which is roughly how steady a crosshair is at those ranges.
                const float reach = gs::Settings::Get().reach;
                // The same cone a press gets. Eight metres and three per cent
                // was for a crosshair swaying in flight, and session
                // seventy-nine says it costs more than it buys: I flew past
                // three glints in a row, aimed at the middle one, and the
                // middle one was the one that never got a pin. At three
                // hundred metres the old cone is nine metres across, wide
                // enough to hold two of the three, and the nearest along the
                // line takes it every time. His presses that session used two
                // metres and one and a half per cent from the same aircraft
                // and they landed where I pointed, so the sway was
                // never the problem the wide cone was solving.
                // Wide, and it stays that way deliberately. Eight metres
                // or the ini's AutoCone, two degrees by default, which is
                // fourteen metres at four hundred. The flash fires while I am
                // flying and the crosshair sways, and a cone that misses here
                // is a glint nobody ever finds again.
                const float autoFrac =
                    std::tan(gs::Settings::Get().autoConeDeg * 3.14159265f / 180.0f);
                tableN = gs::lgso::OnBearing(pp.x, pp.z, v.ox, v.oy, v.oz,
                                             v.fx / flen, v.fz / flen, v.fy / flen,
                                             8.0f, autoFrac, 25.0f,
                                             5.0f, reach > 0.0f ? reach : 1.0e9f,
                                             table, tableAngles, 8);
                // Every node the game has marked, with its distance, so the log
                // says how close the player has to get before the game creates
                // the thing I am looking at.
                if (glintN != g_lastGlintN)
                {
                    g_lastGlintN = glintN;
                    GS_LOG("[auto] the game has marked %d node(s) as detect mode targets", glintN);
                    for (int i = 0; i < glintN; ++i)
                    {
                        const float dx = glints[i].x - pp.x, dz = glints[i].z - pp.z;
                        GS_LOG("[auto]   \"%s\" eid %08X, %.1f metres away, %.1f degrees off the crosshair",
                               glints[i].name[0] ? glints[i].name : "?", glints[i].eid,
                               std::sqrt(dx * dx + dz * dz), glintAngles[i] * 57.2958f);
                    }
                }
                // The whole gimmick set, named, so the log says whether the
                // glint was in it. Once per place: session fifty-three spent
                // both its listings on the first glint and had none left for
                // the one it teleported to.
                if (gs::Settings::Get().verbose && g_setListingsLeft > 0 && now - g_flashOnMs > 700 &&
                    (!g_setListedOnce || FarFrom(pp.x, pp.z, g_setListedX, g_setListedZ, 50.0f)))
                {
                    --g_setListingsLeft;
                    g_setListedOnce = true;
                    g_setListedX = pp.x;
                    g_setListedZ = pp.z;
                    gs::actors::LogEntities(pp.x, pp.z, v.ox, v.oz, v.fx / flen, v.fz / flen);
                }
                if (now - g_flashOnMs > 700) Survey(pp, around, angles, n);
                // And what the game's own detect system is holding. This is
                // the game's own answer to the question the bearing can only
                // guess at, so it is asked on every press and it outranks the
                // guess when it lands on the crosshair.
                gs::aim::Eye eye;
                eye.px = pp.x; eye.py = pp.y; eye.pz = pp.z;
                eye.lx = pp.lx; eye.ly = pp.ly; eye.lz = pp.lz;
                eye.ox = v.ox; eye.oz = v.oz;
                eye.ux = v.fx / flen; eye.uz = v.fz / flen;
                const bool sayTargets = g_targetLogsLeft > 0 && now - g_targetLastMs > 1500;
                if (sayTargets) { --g_targetLogsLeft; g_targetLastMs = now; }
                held = gs::aim::DescribeTargets(eye, sayTargets);
            }
        }
        // Something marked once is not a candidate again, so the next thing on
        // the line gets its turn. Filtering here rather than refusing at the
        // end matters: 1.1.4 refused at the end, and the thing it refused went
        // on winning the pick every pass and taking the shared three second
        // wait with it, so nothing else could be pinned either.
        int markedSeen = 0;
        int pick = -1;
        for (int i = 0; i < n; ++i)
        {
            if (AlreadyMarked(MarkKind::Node, around[i].eid, 0, 0, around[i].x, around[i].z))
            {
                ++markedSeen;
                continue;
            }
            pick = i;
            break;
        }
        float pickAngle = pick >= 0 ? angles[pick] : 0.0f;
        int tablePick = 0;
        while (tablePick < tableN &&
               AlreadyMarked(MarkKind::Table, 0, table[tablePick].record,
                             table[tablePick].element, table[tablePick].x, table[tablePick].z))
        {
            ++tablePick;
            ++markedSeen;
        }
        int glintPick = 0;
        while (glintPick < glintN &&
               AlreadyMarked(MarkKind::Node, glints[glintPick].eid, 0, 0,
                             glints[glintPick].x, glints[glintPick].z))
        {
            ++glintPick;
            ++markedSeen;
        }
        // Seven degrees, because the table is complete and a wide cone over a
        // complete set just invites the wrong answer. At two hundred metres
        // seven degrees is twenty-four metres across, which is about the
        // precision a crosshair has at that range.
        bool byTable = tablePick < tableN;
        if (byTable && g_tableLogsLeft > 0)
        {
            --g_tableLogsLeft;
            const float vlen = std::sqrt(v.fx * v.fx + v.fz * v.fz);
            const float ux = vlen > 1.0e-3f ? v.fx / vlen : 0.0f;
            const float uz = vlen > 1.0e-3f ? v.fz / vlen : 1.0f;
            GS_LOG("[auto] the table has %d placement(s) on the line", tableN);
            for (int k = 0; k < tableN && k < 4; ++k)
            {
                const float ddx = table[k].x - v.ox, ddz = table[k].z - v.oz;
                const float perp = std::fabs(ddx * uz - ddz * ux);
                const float deg = std::atan2(perp, tableAngles[k]) * 57.2958f;
                GS_LOG("[auto]   %srecord %u element %u \"%s\" at (%.1f, %.1f, %.1f), %.0f metres "
                       "out, %.1f off the line, %.2f degrees",
                       k == tablePick ? "TAKEN " : "      ", table[k].record, table[k].element,
                       table[k].name[0] ? table[k].name : "unnamed",
                       table[k].x, table[k].y, table[k].z, tableAngles[k], perp, deg);
            }
        }

        // Fifteen degrees, not forty.
        //
        // Forty was written when the flash lit one thing at a time and the
        // worry was a crosshair swaying at six hundred metres. Standing near a
        // mine it lights three things at once: session 17:05 had ironstone
        // thirteen metres away at twenty-six degrees off, sophora at
        // thirty-three and a spawn point at forty, none of them what I was
        // aiming at, and the ironstone took the pin while I was pointing
        // across a chasm. Sway is a problem at range and not at thirteen
        // metres, where putting the crosshair on a thing is easy, so an angle
        // is the right cap and this one is generous: fifteen degrees is eighty
        // metres across at the far end of what the game keeps loaded.
        constexpr float kGlintCone = 0.262f;   // radians
        bool byGlint = false;
        if (glintPick < glintN && glintAngles[glintPick] < kGlintCone)
        {
            byGlint = true;
            pickAngle = glintAngles[glintPick];
        }
        // Only the glint gets a pin. Session fifty-eight pressed from the spot
        // I have been testing from all along, a hundred and nineteen metres
        // short of my glint, and the bearing picked a bottle four metres away
        // at 1.3 degrees and pinned it. Nothing about that pin was information.
        // The game marks the object it lights; when it has not marked
        // anything, the honest answer is nothing.
        if (!byTable && !byGlint && !gs::Settings::Get().guess)
        {
            if (g_quietLogsLeft > 0 && now - g_quietLastMs > 3000)
            {
                --g_quietLogsLeft;
                g_quietLastMs = now;
                // What the table nearly had, which is the part worth knowing.
                gs::lgso::Place miss;
                float missAlong = 0, missPerp = 0;
                bool missRefused = false;
                const float qlen = std::sqrt(v.fx * v.fx + v.fz * v.fz);
                if (qlen > 1.0e-3f &&
                    gs::lgso::NearestToLine(v.ox, v.oz, v.fx / qlen, v.fz / qlen, 1.0e9f,
                                            &miss, &missAlong, &missPerp, &missRefused))
                    GS_LOG("[auto] nothing pinned. The table's closest to the line is record %u "
                           "element %u \"%s\", %.0f metres out and %.1f metres off the line%s",
                           miss.record, miss.element, miss.name[0] ? miss.name : "unnamed",
                           missAlong, missPerp,
                           missRefused ? ", and the Kinds list refuses it" : "");
                else
                    GS_LOG("[auto] nothing pinned, and the table has nothing on this bearing at all");
            }
            pick = -1;
        }
        if (byGlint && g_glintWinsLeft > 0)
        {
            --g_glintWinsLeft;
            const float gx = glints[glintPick].x - pp.x, gz = glints[glintPick].z - pp.z;
            GS_LOG("[auto] the game has set the glint byte on %d node(s); the nearest the crosshair is "
                   "\"%s\" eid %08X, %.1f metres away, %.1f degrees off. That takes the pin.",
                   glintN, glints[glintPick].name[0] ? glints[glintPick].name : "?",
                   glints[glintPick].eid,
                   std::sqrt(gx * gx + gz * gz), glintAngles[glintPick] * 57.2958f);
        }

        // The game's own detect target, when it has one on the crosshair,
        // beats anything the bearing found. Fifteen degrees is the same cone
        // the nodes are held to, and a target has to be somewhere between five
        // metres and half a kilometre to be a thing the player is looking at.
        //
        // Build 0.6.0 threw this route away for placing a pin "104 units off"
        // in session nineteen. The arithmetic in aim.h says that pin was a
        // hundred and four metres from the player because the target was, and
        // session forty-seven measured a real glint at a hundred and eighteen.
        // Being far away was the evidence against it, and being far away is
        // the whole point.
        // The detect system does not get the pin. Session fifty-three ran the
        // control nobody had run: I aimed at one glint, teleported, and
        // aimed at another. The actor the detect component was holding moved
        // around on its own and sat a hundred and forty-seven degrees off my
        // crosshair, then sixty-eight. It follows the scene and not the aim.
        //
        // The disassembly says why. Every field this route ever read lives
        // past the end of the object it was read from: the task is 112 bytes
        // and the reads were at +0x500 and beyond, the special mode component
        // is 248 and the scan ran to 2048, and the detect component's own code
        // stops at +0x250 while the reads were at +0x508 and +0x3E8. All of it
        // was the neighbouring allocation.
        //
        // It is still read, inside the real bounds now, and still logged, so a
        // build where something real turns up there would say so.
        const bool byTarget = false;

        // Whichever of the two is nearer the crosshair, rather than the table
        // every time.
        //
        // The table holds everything the level has and the glint list holds
        // what the game has lit, and until now the table only lost when it had
        // nothing on the line. That is right when both are pointing at roughly
        // the same place and wrong when they are not: a sealed artifact
        // fourteen hundred metres out and a third of a degree off the line
        // should not lose to a rock at twenty-six degrees, and it did.
        if (byTable && byGlint)
        {
            const float vlen = std::sqrt(v.fx * v.fx + v.fz * v.fz);
            const float ux = vlen > 1.0e-3f ? v.fx / vlen : 0.0f;
            const float uz = vlen > 1.0e-3f ? v.fz / vlen : 1.0f;
            const float ddx = table[tablePick].x - v.ox, ddz = table[tablePick].z - v.oz;
            const float perp = std::fabs(ddx * uz - ddz * ux);
            const float tableRad = std::atan2(perp, tableAngles[tablePick]);
            // Half a degree of hysteresis, so two candidates a hair apart do
            // not swap between passes and reset the hold for ever.
            if (glintAngles[glintPick] + 0.0087f < tableRad) byTable = false;
        }

        // One chosen thing, whichever route named it, so the hold and the pin
        // below do not care which one did.
        struct Chosen
        {
            float x = 0, y = 0, z = 0;
            float angleDeg = 0;
            uint32_t eid = 0;
            uint32_t record = 0;    // a level table placement, when eid is 0
            uint32_t element = 0;
            const char* how = "";
            char name[64]{};
            bool valid = false;
        } chosen;
        if (byTable)
        {
            chosen.x = table[tablePick].x; chosen.y = table[tablePick].y;
            chosen.z = table[tablePick].z;
            chosen.angleDeg = 0.0f;
            chosen.eid = 0;
            chosen.record = table[tablePick].record;
            chosen.element = table[tablePick].element;
            chosen.how = "the game's own level gimmick table";
            _snprintf_s(chosen.name, sizeof(chosen.name), _TRUNCATE, "%s",
                        table[tablePick].name[0] ? table[tablePick].name
                                                 : "unnamed level gimmick");
            chosen.valid = true;
        }
        else if (byGlint)
        {
            chosen.x = glints[glintPick].x; chosen.y = glints[glintPick].y;
            chosen.z = glints[glintPick].z;
            chosen.angleDeg = glintAngles[glintPick] * 57.2958f;
            chosen.eid = glints[glintPick].eid;
            chosen.how = glints[glintPick].how ? glints[glintPick].how : "?";
            strncpy_s(chosen.name, sizeof(chosen.name),
                      glints[glintPick].name[0] ? glints[glintPick].name : "?", _TRUNCATE);
            chosen.valid = true;
        }
        else if (byTarget)
        {
            chosen.x = held.x; chosen.y = held.y; chosen.z = held.z;
            chosen.angleDeg = held.angle;
            chosen.eid = held.eid;
            chosen.how = "the target the game's own detect system is holding";
            _snprintf_s(chosen.name, sizeof(chosen.name), _TRUNCATE, "%s at %s+0x%llX",
                        held.cls[0] == '.' ? held.cls + 4 : held.cls, held.where,
                        static_cast<unsigned long long>(held.at));
            chosen.valid = true;
        }
        else if (pick >= 0)
        {
            chosen.x = around[pick].x; chosen.y = around[pick].y; chosen.z = around[pick].z;
            chosen.angleDeg = angles[pick] * 57.2958f;
            chosen.eid = around[pick].eid;
            chosen.how = around[pick].how ? around[pick].how : "?";
            strncpy_s(chosen.name, sizeof(chosen.name),
                      around[pick].name[0] ? around[pick].name : "?", _TRUNCATE);
            chosen.valid = true;
        }
        if (byTarget && g_heldWinsLeft > 0)
        {
            --g_heldWinsLeft;
            GS_LOG("[auto] the game says it is holding %s, %.1f metres away, %.1f degrees off the "
                   "crosshair; that outranks %s",
                   chosen.name, held.dist, held.angle,
                   pick >= 0 ? "the node the bearing found" : "an empty bearing search");
        }

        if (g_autoLogsLeft > 0 && now - g_autoLastLogMs > 2000)
        {
            g_autoLastLogMs = now;
            --g_autoLogsLeft;
            GS_LOG("[auto] flash on at (%.1f, %.1f, %.1f); %d marked nodes loaded, %d candidate node(s) in radius, "
                   "%d within fifteen degrees of the crosshair; the farthest marked node is %.0f metres out",
                   pp.x, pp.y, pp.z, gs::actors::PickupCount(), marked, n,
                   gs::actors::MarkedReach(pp.x, pp.z));
            for (int i = 0; i < n && i < 5; ++i)
            {
                const float dx = around[i].x - pp.x, dz = around[i].z - pp.z;
                GS_LOG("[auto]   %s\"%s\" eid %08X %.1f degrees off, %.1f away at (%.1f, %.1f, %.1f)",
                       i == pick ? "ON THE CROSSHAIR " : "",
                       around[i].name[0] ? around[i].name : "?", around[i].eid, angles[i] * 57.2958f,
                       std::sqrt(dx * dx + dz * dz), around[i].x, around[i].y, around[i].z);
            }
            if (pick < 0) GS_LOG("[auto]   no node within fifteen degrees of the crosshair");
            if (held.valid)
                GS_LOG("[auto]   the game's detect system holds %s at %.1f metres, %.1f degrees off, %s%s",
                       held.cls[0] == '.' ? held.cls + 4 : held.cls, held.dist, held.angle,
                       held.inPools ? "live in the pools" : "not in the pools",
                       byTarget ? ", and it takes the pick" : ", so the bearing keeps the pick");
            else GS_LOG("[auto]   the game's detect system is holding nothing the mod can resolve");
        }

        // One pin, on the place the crosshair held for a second. The hold
        // is by position and not by object: session forty-six aimed at a
        // patch of berry bushes and the nearest by bearing swapped between
        // neighbours every pass, so a hold keyed on the object's id never
        // reached a second.
        if (!chosen.valid)
        {
            g_heldEid = 0;
            g_heldSinceMs = 0;
        }
        else
        {
            // Six metres was right for things twenty metres away. A
            // placement four hundred metres out moves further than that
            // between passes while the crosshair sways, and resetting the
            // timer every time means no pin ever matures.
            const float hx = chosen.x - g_heldX, hz = chosen.z - g_heldZ;
            const float hd = std::sqrt((chosen.x - pp.x) * (chosen.x - pp.x) +
                                       (chosen.z - pp.z) * (chosen.z - pp.z));
            const float holdTol = hd * 0.05f > 10.0f ? hd * 0.05f : 10.0f;
            const bool samePlace = g_heldSinceMs && std::sqrt(hx * hx + hz * hz) <= holdTol;
            if (!samePlace) g_heldSinceMs = now;
            g_heldEid = chosen.eid;
            g_heldX = chosen.x;
            g_heldZ = chosen.z;
        }
        // There is no ceiling unless the ini asks for one. Half a kilometre
        // was written down when the only glint anyone had measured sat a
        // hundred and nineteen metres away, session seventy-five caught it
        // refusing a correct pick at five hundred and ninety-eight, and the
        // twelve hundred that replaced it was the same mistake with a bigger
        // number. The pick is already decided by distance from the line, so
        // a limit here does nothing but throw away right answers.
        const float chosenDist = chosen.valid
            ? std::sqrt((chosen.x - pp.x) * (chosen.x - pp.x) + (chosen.z - pp.z) * (chosen.z - pp.z))
            : 0.0f;
        const float ceiling = gs::Settings::Get().reach;
        if (chosen.valid && ceiling > 0.0f && chosenDist > ceiling)
        {
            GS_LOG("[auto] \"%s\" resolved %0.f metres away, past the ini's Reach of %.0f; not pinned",
                   chosen.name, chosenDist, ceiling);
            chosen.valid = false;
        }
        // A pin already within eight metres suppresses this one, and until
        // now it did so in silence. That silence is indistinguishable from a
        // broken feature: aim at a glint you pinned ten minutes ago, hold it,
        // and the mod does nothing and says nothing.
        const bool matured = chosen.valid && g_heldSinceMs && now - g_heldSinceMs >= 1000;
        if (matured && now >= g_cooldownUntil && gs::mapicon::PinNear(chosen.x, chosen.z, kPinApart) &&
            g_dupLogsLeft > 0 && now - g_dupLastMs > 3000)
        {
            --g_dupLogsLeft;
            g_dupLastMs = now;
            GS_LOG("[auto] \"%s\" is already pinned; the map has it from earlier this session",
                   chosen.name);
        }
        // Everything on the line has been marked once already and there was
        // nothing else to take. Said out loud, because a flash that does
        // nothing and says nothing reads as broken.
        if (!chosen.valid && markedSeen > 0 && g_markedLogsLeft > 0 &&
            now - g_markedLastMs > 10000)
        {
            --g_markedLogsLeft;
            g_markedLastMs = now;
            GS_LOG("[auto] everything on the line was marked by this flash already. Press the "
                   "flash again, or the button, for another pin on the same thing.");
        }
        if (matured && now >= g_cooldownUntil &&
            !gs::mapicon::PinNear(chosen.x, chosen.z, kPinApart))
        {
            // The ground, asked once, at the moment a pin is about to land.
            //
            // I marked a glint through a mountain. This cannot see the
            // whole way there, because collision only exists in a box around
            // me, but a mountain between me and something a kilometre off is
            // usually much nearer than the thing itself and so usually inside
            // that box. Sixteen samples is about twenty milliseconds and it
            // happens once per pin, not once per pass.
            {
                const float eye[3] = {v.ox, v.oy, v.oz};
                const float tgt[3] = {chosen.x, chosen.y, chosen.z};
                float blockedAt = 0.0f;
                const gs::physics::Sight s =
                    gs::physics::LineOfSight(eye, tgt, 16, &blockedAt);
                if (s == gs::physics::Sight::Blocked)
                {
                    if (g_losLogsLeft > 0 && now - g_losLastMs > 3000)
                    {
                        --g_losLogsLeft;
                        g_losLastMs = now;
                        GS_LOG("[auto] \"%s\" is behind ground that rises across the sight line "
                               "%.0f metres out; not pinned", chosen.name, blockedAt);
                    }
                    g_heldSinceMs = now;   // start the hold again rather than spin
                    return;
                }
            }

            g_cooldownUntil = now + 3000;
            const float dx = chosen.x - pp.x, dz = chosen.z - pp.z;
            GS_LOG("[auto] the crosshair held \"%s\" eid %08X for a second, %.1f degrees off, %.1f metres away; pinning it where it stands",
                   chosen.name, chosen.eid, chosen.angleDeg, std::sqrt(dx * dx + dz * dz));
            {
                const float north = chosen.z - pp.z, east = chosen.x - pp.x;
                GS_LOG("[auto]   its position came from %s; that is %.0f metres %s and %.0f metres %s of you",
                       chosen.how, std::fabs(north), north >= 0 ? "north" : "south",
                       std::fabs(east), east >= 0 ? "east" : "west");
            }
            const bool landed =
                PlaceAt(chosen.x, chosen.y, chosen.z,
                        byTable ? "automatic, the game's own level gimmick table"
                                : byGlint ? "automatic, the node the game marked as a detect mode target"
                                : (byTarget ? "automatic, what the game's detect system is holding"
                                            : "automatic, the node under the crosshair"),
                        "Glint", pp, kPinApart);
            if (landed)
                RememberMarked(byTable ? MarkKind::Table : MarkKind::Node, chosen.eid,
                               chosen.record, chosen.element, chosen.x, chosen.z);
        }

        // The measurement, once the flash has been on for a moment.
        if (g_huntLeft > 0 && n > 0 && !g_huntN && now - g_flashOnMs > 1200 && now - g_huntLastMs > 10000)
        {
            g_huntLastMs = now;
            --g_huntLeft;
            g_huntN = n < 4 ? n : 4;
            for (int i = 0; i < g_huntN; ++i)
            {
                g_huntEnts[i] = around[i].ptr;
                g_huntEids[i] = around[i].eid;
                strncpy_s(g_huntNames[i], sizeof(g_huntNames[i]), around[i].name[0] ? around[i].name : "?", _TRUNCATE);
                g_huntOn[i] = gs::dump::EffectActivity(around[i].ptr, 0x400);
                char tag[24];
                _snprintf_s(tag, sizeof(tag), _TRUNCATE, "on%d", i + 1);
                GS_LOG("[hunt] \"%s\" eid %08X with the flash on, %d effects", g_huntNames[i], g_huntEids[i], g_huntOn[i]);
                gs::dump::GimmickState(tag, around[i].ptr, 0x480);
            }
            GS_LOG("[hunt] hold the flash on the one that glows, then let it end; the same four are read again");
        }
    }
}

namespace
{
    // The map asked for one of the mod's markers to go. The request went to
    // the server, which has never heard of it, so the mod erases its own
    // record and takes the icon off both surfaces.
    void DrainRetires()
    {
        int64_t retire[8];
        const int n = gs::realpin::TakeRetired(retire, 8);
        for (int i = 0; i < n; ++i)
        {
            // Where it stood first, because taking the icon off runs through
            // the mod's own spy on that slot and the answer is gone after.
            float gx = 0.0f, gz = 0.0f;
            const bool known = gs::mapicon::Forget(retire[i], &gx, &gz);
            gs::realpin::Retire(retire[i]);
            gs::mapicon::RemoveIcon(gs::mapicon::LastWorldRoot(), retire[i]);
            if (known)
            {
                gs::pinstore::Drop(gx, gz);
                SettleAt(gx, gz);
            }
        }
    }

    // Slot 35 on the world map root, the same per-frame update the minimap
    // carries. Plain C++ rather than the assembler thunk the minimap uses:
    // this one only reads a queue and forwards, and the signature is two
    // arguments the compiler can be trusted with.
    using UpdateFn = void (*)(void*, float);
    gs::vtable::Swap g_worldSwap;
    UpdateFn g_worldOrig = nullptr;
    std::atomic<uint64_t> g_worldTicks{0};

    void WorldMapUpdate(void* self, float dt)
    {
        const uint64_t n = ++g_worldTicks;
        if (n == 1)
            GS_LOG_OK("[tick] first world map update on thread %lu, root 0x%p; this is the one "
                      "that runs while the map is open", GetCurrentThreadId(), self);
        if ((n % 8) == 0) DrainRetires();
        if (g_worldOrig) g_worldOrig(self, dt);
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
        PumpSaveEvents();
        RestoreOnNewWorld();
        FlushPending();
        // A map that has just been rebuilt has none of the mod's pins on it.
        DrainRetires();
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
    // The pin lands where the view ray goes: the first object on it, or the
    // ground under it when the camera is looking down. Never at the player.
    if (g_markPending.exchange(false))
    {
        // A press needs a position and a direction. It does not need the
        // detect component, which is only there to tell the automatic marker
        // when the flash is up, and it does not need the player's own
        // component either, because the camera carries the same position in
        // both frames from the first tick.
        //
        // That matters because the actor manager takes sixty to seventy-five
        // seconds to hand the component over, and 0.49.0 stopped walking the
        // heap to get it sooner. Waiting that long to be able to press was the
        // price of not freezing, and this is how the price gets paid back.
        gs::player::Pos pp = gs::player::Read();
        if (!pp.valid)
        {
            const gs::camera::Pose cam = gs::camera::Read();
            if (cam.valid && cam.worldValid)
            {
                pp.x = cam.world[0]; pp.y = cam.world[1]; pp.z = cam.world[2];
                pp.lx = cam.pivot[0]; pp.ly = cam.pivot[1]; pp.lz = cam.pivot[2];
                pp.ox = pp.x - pp.lx; pp.oy = pp.y - pp.ly; pp.oz = pp.z - pp.lz;
                pp.q[0] = cam.q[0]; pp.q[1] = cam.q[1]; pp.q[2] = cam.q[2]; pp.q[3] = cam.q[3];
                pp.valid = true;
                pp.fromCamera = true;
            }
        }
        if (!pp.valid)
        {
            GS_LOG_ERR("[mark] NOT READY: neither the player's own component nor the camera has a "
                       "position yet. Give it a few seconds after the world appears.");
            return;
        }
        if (pp.fromCamera)
            GS_LOG("[mark] the player's own component is not in hand yet, so this press is using "
                   "the camera's position: world (%.1f, %.1f, %.1f), sub-level origin "
                   "(%.0f, %.0f, %.0f)", pp.x, pp.y, pp.z, pp.ox, pp.oy, pp.oz);
        const bool flash = gs::aim::FlashActive();
        GS_LOG("[mark] requested. player world (%.3f, %.3f, %.3f) local (%.3f, %.3f, %.3f), flash %s",
               pp.x, pp.y, pp.z, pp.lx, pp.ly, pp.lz, flash ? "on" : "off");

        float tx = 0, ty = 0, tz = 0;
        bool have = false;
        const char* how = "";
        char markLabel[16] = "Mark";

        // The table answers first, and it answers at any range.
        //
        // Everything below this block reads the collision world, which the
        // engine only keeps loaded in a box about a hundred and sixty metres
        // across. That is why a press used to mark whatever was underfoot.
        // The level gimmick table has no such limit: seventeen thousand
        // placements over the whole map, read once from a fixed global.
        // Session seventy-five proved the aim holds up out there, matching
        // the camera yaw to a fifth of a degree against a placement five
        // hundred and ninety-eight metres away.
        //
        // The Kinds filter is off here on purpose. Glint hunting needs it.
        // A deliberate press is the player saying "that thing over there",
        // and that thing is as likely to be a bridge or a camp as a glint.
        {
            View sv;
            // Never loaded from here. Reading the table walks seventeen
            // thousand records and takes about a second and a half, and this
            // runs on the thread drawing the frame. The worker does it once
            // at startup; a press before that says so and falls through.
            if (gs::lgso::Count() == 0)
                GS_LOG("[mark] the level gimmick table has not been read yet; wait for the line "
                       "saying how many placements it holds");
            if (gs::lgso::Count() > 0 && ViewRay(pp, &sv))
            {
                const float flen = std::sqrt(sv.fx * sv.fx + sv.fz * sv.fz);
                if (flen > 1.0e-4f)
                {
                    gs::lgso::Place sight[4];
                    float sightDist[4];
                    // A rod: the ini's Rod metres either side of the line, the
                    // same at every distance, which is what a growth rate of
                    // zero means here.
                    //
                    // Every cone this feature tried was widest exactly where
                    // the table is densest, and every wrong pin it ever placed
                    // landed further away than the thing I was aiming at. A
                    // constant radius is the shape that cannot do that.
                    //
                    // Twenty-five metres is still the closest a press looks. A
                    // map marker for something four paces away is not a thing
                    // anybody wants.
                    //
                    // No ceiling, whatever Reach says. Reach exists because the
                    // automatic path pins on its own and once put a marker
                    // three kilometres out during a flash. A press is the
                    // player asking for a specific thing, and if they can see a
                    // tower across the map they can have it.
                    const float pressReach = gs::Settings::Get().pressReach;
                    const int sn = gs::lgso::OnBearing(pp.x, pp.z, sv.ox, sv.oy, sv.oz,
                                                       sv.fx / flen, sv.fz / flen, sv.fy / flen,
                                                       gs::Settings::Get().rodMetres, 0.0f, 25.0f,
                                                       25.0f,
                                                       pressReach > 0.0f ? pressReach : 1.0e9f,
                                                       sight, sightDist, 4, true);

                    // And, whatever the rod said, the eight things closest to
                    // the sight line. Five apertures have been tried without
                    // anyone checking whether the thing being pointed at is in
                    // the table at all. If it is, it shows up here a few
                    // centimetres off the line and the aperture is the
                    // problem. If everything within three hundred metres is
                    // twenty metres off the line, no aperture was ever going
                    // to find it.
                    {
                        // Not called near: windef.h defines that as a macro.
                        gs::lgso::Place onLine[8];
                        float onAlong[8], onPerp[8];
                        const int nn = gs::lgso::NearLine(sv.ox, sv.oz, sv.fx / flen,
                                                          sv.fz / flen, 3000.0f,
                                                          onLine, onAlong, onPerp, 8);
                        // Band by band first, because a gap in the table has
                        // to look like a gap and not like an absence of luck.
                        static const float kBands[] = {25.0f, 60.0f, 120.0f, 250.0f, 500.0f,
                                                       1000.0f, 2000.0f, 4000.0f};
                        GS_LOG("[mark] the sight line, band by band:");
                        gs::lgso::LogBands(sv.ox, sv.oy, sv.oz, sv.fx / flen, sv.fz / flen,
                                           sv.fy / flen, kBands, 7);
                        GS_LOG("[mark] closest to the sight line overall, whatever the rod or "
                               "the height test says (a placement is only taken when it is "
                               "within %.1f m of the line and within the greater of 20 m or four "
                               "per cent of its range in height):",
                               gs::Settings::Get().rodMetres);
                        for (int k = 0; k < nn; ++k)
                        {
                            const float lineY = sv.oy + (sv.fy / flen) * onAlong[k];
                            GS_LOG("[mark]   %.1f m off the line, %.0f m out, %+.0f m in height, "
                                   "record %u element %u \"%s\" at (%.1f, %.1f, %.1f)",
                                   onPerp[k], onAlong[k], onLine[k].y - lineY,
                                   onLine[k].record, onLine[k].element,
                                   onLine[k].name[0] ? onLine[k].name : "unnamed",
                                   onLine[k].x, onLine[k].y, onLine[k].z);
                        }
                    }
                    if (sn > 0)
                    {
                        tx = sight[0].x; ty = sight[0].y; tz = sight[0].z;
                        have = true;
                        how = "the game's own level gimmick table";
                        _snprintf_s(markLabel, sizeof(markLabel), _TRUNCATE, "%.0fm", sightDist[0]);
                        {
                            const float eye[3] = {sv.ox, sv.oy, sv.oz};
                            const float tgt[3] = {tx, ty, tz};
                            float blockedAt = 0.0f;
                            const gs::physics::Sight s =
                                gs::physics::LineOfSight(eye, tgt, 24, &blockedAt);
                            // A press is deliberate, so it is told, not
                            // overruled. If you point at a ridge and ask for
                            // what is behind it, that is yours to ask.
                            if (s == gs::physics::Sight::Blocked)
                                GS_LOG("[mark] note: ground rises across the sight line %.0f metres "
                                       "out, so this is behind a hill", blockedAt);
                        }
                        const float ux = sv.fx / flen, uz = sv.fz / flen;
                        for (int k = 0; k < sn; ++k)
                        {
                            const float ddx = sight[k].x - sv.ox, ddz = sight[k].z - sv.oz;
                            const float perp = std::fabs(ddx * uz - ddz * ux);
                            const float deg = std::atan2(perp, sightDist[k]) * 57.2958f;
                            GS_LOG("[mark]   %srecord %u element %u \"%s\" at (%.1f, %.1f, %.1f), "
                                   "%.0f metres out, %.1f off the line, %.2f degrees",
                                   k == 0 ? "TAKEN " : "      ", sight[k].record, sight[k].element,
                                   sight[k].name[0] ? sight[k].name : "unnamed",
                                   sight[k].x, sight[k].y, sight[k].z, sightDist[k], perp, deg);
                        }
                    }
                    else
                    {
                        gs::lgso::Place miss;
                        float missAlong = 0, missPerp = 0;
                        bool missRefused = false;
                        if (gs::lgso::NearestToLine(sv.ox, sv.oz, sv.fx / flen, sv.fz / flen,
                                                    1.0e9f, &miss, &missAlong, &missPerp,
                                                    &missRefused))
                            GS_LOG("[mark] nothing within %.2f metres of the line. The closest the "
                                   "table has is record %u element %u \"%s\" at (%.1f, %.1f, %.1f), "
                                   "%.0f metres out, %.1f off the line, %.2f degrees",
                                   gs::Settings::Get().rodMetres, miss.record, miss.element,
                                   miss.name[0] ? miss.name : "unnamed", miss.x, miss.y, miss.z,
                                   missAlong, missPerp,
                                   std::atan2(missPerp, missAlong) * 57.2958f);
                        else
                            GS_LOG("[mark] the table has nothing on this bearing at all");
                        GS_LOG("[mark] falling back to the collision world, which reaches about "
                               "eighty metres");
                    }
                }
            }
        }

        // The game's own aim field, logged when present. Populated in session
        // seventeen and zero in eighteen and nineteen, so it is a hint, not
        // the aim.
        float a[3];
        if (gs::aim::AimPointLocal(gs::player::CharacterControlComponent(), pp.lx, pp.ly, pp.lz, a))
            GS_LOG("[mark] game aim field local (%.2f, %.2f, %.2f) -> world (%.2f, %.2f, %.2f)",
                   a[0], a[1], a[2], a[0] + pp.ox, a[1] + pp.oy, a[2] + pp.oz);

        // Seventy-seven lines a press, most of them the camera object and its
        // owner dumped whole. That was how the camera's fields were named and
        // it has no business running on every press of a working feature.
        if (gs::Settings::Get().verbose)
            gs::camera::LogAtPress(2.0f * std::atan2(pp.q[1], pp.q[3]));

        // Read only, and only a few times. I could not delete the mod's pins
        // because they were never markers, and this is the first look at where
        // real ones live.
        // Microseconds now: four hundred bytes of header reads at a computed
        // offset, where session eighty-nine swept megabytes for five hundred
        // and eighty milliseconds on this thread, once per press.
        if (g_pinModelLogsLeft > 0)
        {
            --g_pinModelLogsLeft;
            gs::pinmodel::LogState("on a press");
            gs::realpin::LogState("before the press");
        }

        View v;
        if (have)
            GS_LOG("[mark] the table answered, so the world ray is not cast");
        else if (!gs::Settings::Get().rayFallback)
            GS_LOG("[mark] the table could not answer and RayFallback is off, so nothing is "
                   "placed. The list above says what was near the line.");
        else if (!gs::actors::Ready())
            GS_LOG("[mark] actor manager not located yet, no ray");
        else if (gs::actors::Count() == 0)
            GS_LOG("[mark] actor set is empty, no ray");
        else if (!ViewRay(pp, &v))
            GS_LOG("[mark] facing quaternion (%.3f, %.3f, %.3f, %.3f) is not a yaw and no camera, no ray",
                   pp.q[0], pp.q[1], pp.q[2], pp.q[3]);
        else
        {
            // A real ray against the world's collision, from the camera along
            // the view. Session thirty: layer 0 hit a vertical face 74 units
            // out that was not the ground I pointed at. So every press
            // casts on every candidate layer word, with the flag both ways,
            // and logs each hit; a ground-like hit (normal pointing up) is
            // preferred, then the farthest hit. A hit nearer than the pivot
            // is the player's own body and the ray is cast again from just
            // past the pivot.
            const gs::camera::Pose cam = gs::camera::Read();
            const float camDist = (cam.valid && cam.dist > 0.5f && cam.dist < 30.0f) ? cam.dist : 6.0f;
            gs::physics::LogState();
            const float origin[3] = {v.ox, v.oy, v.oz};
            const float dir[3] = {v.fx, v.fy, v.fz};
            gs::physics::Hit best;
            int usedLayer = -1;
            float bestScore = -1e9f;
            const int layers[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 58,
                                  0x3B, 0x3C, 0x3D, 0x3E, 0x40, 0x44, 0x48};
            for (int fl = 0; fl < 2; ++fl)
            for (int li = 0; li < static_cast<int>(sizeof(layers) / sizeof(layers[0])); ++li)
            {
                const int layer = layers[li];
                const bool flag = fl == 1;
                gs::physics::Hit h = gs::physics::Cast(origin, dir, 600.0f, layer, flag);
                if (h.hit && h.dist < camDist + 1.0f)
                {
                    const float o2[3] = {v.ox + v.fx * (camDist + 1.0f), v.oy + v.fy * (camDist + 1.0f), v.oz + v.fz * (camDist + 1.0f)};
                    gs::physics::Hit h2 = gs::physics::Cast(o2, dir, 600.0f, layer, flag);
                    if (h2.hit) { h2.dist += camDist + 1.0f; h = h2; }
                    else h.hit = false;
                }
                if (!h.hit) continue;
                GS_LOG("[mark] layer %d flag %d: hit at %.1f, normal (%.2f, %.2f, %.2f), out flag %d, lands (%.1f, %.1f, %.1f)",
                       layer, flag ? 1 : 0, h.dist, h.normal[0], h.normal[1], h.normal[2], h.flag ? 1 : 0,
                       v.ox + v.fx * h.dist, v.oy + v.fy * h.dist, v.oz + v.fz * h.dist);
                // Sessions thirty and thirty-one: every direct hit sat on a face
                // of a 160 by 160 box around the player with a normal along
                // an axis. That is the edge of the loaded collision, not a
                // surface, and it does not count.
                const bool edge = std::fabs(h.normal[1]) < 0.05f &&
                                  (std::fabs(std::fabs(h.normal[0]) - 1.0f) < 0.01f || std::fabs(std::fabs(h.normal[2]) - 1.0f) < 0.01f);
                if (edge) continue;
                // Ground-like first, then far.
                const float score = (h.normal[1] > 0.3f ? 10000.0f : 0.0f) + h.dist;
                if (score > bestScore) { bestScore = score; best = h; usedLayer = layer; }
            }
            static int knownLayer = -1;

            // Sessions thirty and thirty-one: the direct ray stops at a face
            // square to it, at the same z plane both times, on every layer
            // that hits at all. So the ground is found another way: probes
            // straight down along the view, each from well above the ray,
            // and the ground point is where the terrain first rises to meet
            // the ray. A probe that finds nothing says collision is not
            // loaded there, which is its own answer.
            float probeT = -1, probeY = 0;
            {
                const float down[3] = {0, -1, 0};
                int misses = 0, probes = 0;
                float lastLoggedT = -100;
                float t1 = -1, h1 = 0, t2 = -1, h2 = 0;   // the last two probes that found ground
                for (float t = 4.0f; t <= 400.0f && probeT < 0; t += (t < 60.0f ? 4.0f : 8.0f))
                {
                    const float px = v.ox + v.fx * t, pz = v.oz + v.fz * t, rayY = v.oy + v.fy * t;
                    const float from[3] = {px, rayY + 150.0f, pz};
                    const gs::physics::Hit h = gs::physics::Cast(from, down, 900.0f, 0, false);
                    ++probes;
                    if (!h.hit) { ++misses; if (t - lastLoggedT >= 40.0f) { lastLoggedT = t; GS_LOG("[mark]   probe %.0f out: nothing below", t); } continue; }
                    const float groundY = from[1] - h.dist;
                    t1 = t2; h1 = h2; t2 = t; h2 = groundY;
                    if (t - lastLoggedT >= 40.0f || groundY >= rayY - 0.2f)
                    {
                        lastLoggedT = t;
                        GS_LOG("[mark]   probe %.0f out: ground at %.1f, ray at %.1f, normal (%.2f, %.2f, %.2f)", t, groundY, rayY,
                               h.normal[0], h.normal[1], h.normal[2]);
                    }
                    if (groundY >= rayY - 0.2f) { probeT = t; probeY = groundY; }
                }
                GS_LOG("[mark] %d probes, %d found nothing below; %s", probes, misses,
                       probeT > 0 ? "the ground meets the view ray" : "the view ray never meets the ground where collision is loaded");
                // Beyond the loaded window, carry the last slope forward until it
                // meets the ray. Wrong on a hill, right on a gentle fall, and
                // better than the window's edge.
                if (probeT < 0 && t1 > 0 && t2 > t1)
                {
                    const float slope = (h2 - h1) / (t2 - t1);
                    const float denom = v.fy - slope;
                    if (denom < -1e-4f)
                    {
                        const float t = (h2 - slope * t2 - v.oy) / denom;
                        if (t > t2 && t <= 500.0f)
                        {
                            probeT = t;
                            probeY = h2 + slope * (t - t2);
                            GS_LOG("[mark] beyond the window: the ground slope %.3f from the last probes (%.0f: %.1f, %.0f: %.1f) meets the ray %.0f units out at height %.1f (extrapolated)",
                                   slope, t1, h1, t2, h2, t, probeY);
                        }
                        else GS_LOG("[mark] beyond the window: the slope meets the ray at %.0f, out of range", t);
                    }
                    else GS_LOG("[mark] beyond the window: the ground falls away faster than the ray; no meeting point");
                }
            }
            if (probeT > 0)
            {
                best.hit = true;
                best.dist = probeT;
                usedLayer = knownLayer >= 0 ? knownLayer : 0;
                GS_LOG("[mark] the probes put the ground point %.0f units out at height %.1f; the direct ray said %s %.1f",
                       probeT, probeY, bestScore > -1e8f ? "a hit at" : "nothing", bestScore > -1e8f ? best.dist : 0.0f);
            }
            if (best.hit)
            {
                knownLayer = usedLayer;
                tx = v.ox + v.fx * best.dist; ty = v.oy + v.fy * best.dist; tz = v.oz + v.fz * best.dist;
                have = true;
                how = "the world ray";
                GS_LOG("[mark] chosen: layer %d, the world ray lands %.1f units out at (%.1f, %.1f, %.1f)", knownLayer, best.dist, tx, ty, tz);
            }
            else
            {
                GS_LOG("[mark] the world ray hit nothing on any layer; falling back to the terrain estimate");
                float gx = 0, gy = 0, gz = 0, gt = 0;
                uint32_t geid = 0;
                bool ground = v.camera && gs::nearest::GroundAlong(v.ox, v.oy, v.oz, v.fx, v.fy, v.fz, 300.0f,
                                                                    &gx, &gy, &gz, &gt, &geid);
                if (!ground) ground = GroundPoint(v, &gx, &gy, &gz, &gt);
                if (ground)
                {
                    have = true; tx = gx; ty = gy; tz = gz;
                    how = "the terrain estimate under the view ray";
                }
                else GS_LOG("[mark] no ground point either");
            }
        }

        // The detect component's scalars, kept in the log for the record.
        {
            const uintptr_t d = gs::player::DetectComponent();
            if (d && gs::rtti::Readable(reinterpret_cast<const void*>(d), 0x650))
            {
                const auto* q = reinterpret_cast<const uint8_t*>(d);
                float f3ec, f580, f300;
                uint32_t u410, u42c;
                memcpy(&f3ec, q + 0x3EC, 4); memcpy(&f580, q + 0x580, 4); memcpy(&f300, q + 0x300, 4);
                memcpy(&u410, q + 0x410, 4); memcpy(&u42c, q + 0x42C, 4);
                GS_LOG("[mark] detect scalars: +300 %.3f +3EC %.3f +580 %.3f +410 0x%X +42C 0x%X", f300, f3ec, f580, u410, u42c);
            }
        }

        if (have) PlaceAt(tx, ty, tz, how, markLabel, pp, 2.0f);
        else GS_LOG("[mark] no target resolved, nothing placed");
        (void)g_markX; (void)g_markZ; (void)g_markLabel;
    }
}

namespace gs::tick
{
    bool TakeWorldRebuilt() { return g_worldRebuilt.exchange(false); }

    bool InstallWorldMap(uintptr_t worldVtable)
    {
        if (!worldVtable) return false;
        if (!gs::vtable::Install(worldVtable, gs::sig::kSlotUpdate,
                                 reinterpret_cast<void*>(&WorldMapUpdate), g_worldSwap))
        {
            GS_LOG_ERR("[tick] slot %d on the world map vtable could not be taken",
                       gs::sig::kSlotUpdate);
            return false;
        }
        g_worldOrig = reinterpret_cast<UpdateFn>(g_worldSwap.original);
        GS_LOG_OK("[tick] slot %d on the world map vtable was 0x%p, now ours; a marker removed "
                  "while the map is open is answered from there", gs::sig::kSlotUpdate,
                  g_worldSwap.original);
        return true;
    }

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

    void SetWorldRoot(void* root)
    {
        // Kept for the log. No call is ever made on it; see PlaceAt.
        g_worldRoot.store(root);
    }
}
