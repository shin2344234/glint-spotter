#pragma once
#include <cstdint>
#include <vector>

// Finding a live object by its vtable pointer.
//
// Every instance of a polymorphic class carries its vtable address in the first
// eight bytes, so a class with a known vtable can be found without hooking
// anything. That matters here: the map icon create call needs a pointer to the
// world map or minimap root control, and the only other way to get one is to
// detour the per frame update, which means guessing an argument list and taking
// a vtable slot another mod already owns.
//
// This is read-only. It never writes to the game and never installs a detour.
//
// Two things the first session taught, both in the filters below. A bare pointer
// match is mostly noise: the first run returned five hits and every one of them
// sat within a few hundred bytes of the end of an 8 KB region, far too close to
// the edge to hold a 3144-byte object. And walking every committed page cost 58
// seconds and still ran out of budget before it had seen everything, because most
// of a game's committed memory is asset pools that cannot contain a UI control.

namespace gs::scan
{
    struct Hit
    {
        void* object = nullptr;   // the candidate, vtable pointer at offset 0
        int needle = -1;          // which entry of the needles array matched
        uintptr_t regionBase = 0;
        size_t regionSize = 0;
    };

    struct Options
    {
        // A hit is only reported when this many bytes fit between it and the end
        // of its own region. The whole region is already known committed and
        // readable, so this is the fit test and the readable test at once.
        // Per needle, because a root control is 3144 bytes and an icon is not.
        const size_t* needleBytes = nullptr;

        // Widen past the heap. Off is private, committed, read-write, which is
        // where a heap object lives and is fast. On also takes mapped regions,
        // copy-on-write and executable-writable pages, for when the narrow pass
        // came back empty and coverage matters more than the clock.
        bool wideKinds = false;

        // Regions bigger than this are skipped. A UI control comes from the
        // general allocator; the multi-gigabyte regions are asset pools.
        // Report::bytesSkippedLarge says how much this threw away, so a miss can
        // be told apart from a bad threshold.
        size_t maxRegionBytes = 256ull * 1024 * 1024;

        // Read only pages already in the working set. A committed page that is
        // not resident costs a soft fault to touch, which is roughly a fortieth
        // of the throughput, and an object the game is actively drawing with is
        // resident by definition. Turn this off only to prove something is
        // genuinely absent rather than merely paged out.
        bool residentOnly = true;

        // Wall clock ceiling. Time is what the player notices, not bytes.
        uint64_t timeBudgetMs = 4000;

        // Asked once per region; true ends the walk there. For a walk whose
        // answer can turn up some other way while it runs.
        bool (*stop)() = nullptr;

        size_t maxHits = 256;

        // Regions smaller than this are skipped. The tables that pair vtables
        // with their names live in regions of 8 to 48 KB; heap arenas are tens
        // of megabytes. A single-needle scan cannot use the many-classes test,
        // so this is how it avoids them. Zero means no floor.
        size_t minRegionBytes = 0;

        // Scan only the region containing this address. For the self test,
        // which knows exactly where its canary is.
        uintptr_t onlyRegionContaining = 0;

        // Give the machine back after this many bytes, for this long.
        //
        // The scan runs on a worker thread, so in principle the game keeps its
        // own. In practice reading five gigabytes as fast as the memory
        // controller allows is a freeze, because the game is competing for
        // bandwidth and for the pages this walk is faulting in. I have felt
        // it as a hitch the moment the mod comes alive, through several
        // sessions and several theories about what else it might be.
        //
        // A millisecond every four megabytes was tried and made it worse:
        // the walk took the same seventeen seconds of work and spread it over
        // longer, so the hitch lasted longer at the same intensity. Whatever
        // the game is contending for, it is not something a sleeping thread
        // gives back. Off by default; the knob stays because the measurement
        // was worth keeping and the next theory may want it.
        size_t yieldEveryBytes = 0;
        uint32_t yieldMs = 1;
    };

    struct Report
    {
        size_t regionsSeen = 0;
        size_t regionsScanned = 0;
        size_t regionsSkippedLarge = 0;
        size_t regionsSkippedKind = 0;   // wrong state, type or protection
        size_t regionsSkippedSmall = 0;  // cannot hold the object at all
        uint64_t bytesScanned = 0;
        uint64_t bytesSkippedLarge = 0;
        uint64_t bytesSkippedNotResident = 0;
        size_t rawMatches = 0;           // pointer matched
        size_t rejectedNoRoom = 0;       // matched but too near the end of its region
        uint64_t microseconds = 0;
        bool stopped = false;            // Options::stop asked for it
        bool timeBudgetHit = false;
    };

    // Every private, committed, read-write location holding one of these exact
    // pointer values with room for a whole object behind it. One walk covers
    // every needle, so never call this once per class.
    Report FindPointers(const uintptr_t* needles, size_t needleCount,
                        std::vector<Hit>& out, const Options& opt);

    // Cheap re-check of a pointer a previous scan returned: is it still readable
    // and does it still carry this vtable. Microseconds, so it is what the steady
    // state should call instead of scanning again.
    bool StillValid(const void* object, uintptr_t vtableAddress);
}
