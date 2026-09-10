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
        size_t objectBytes = 0;

        // Regions bigger than this are skipped. A UI control comes from the
        // general allocator; the multi-hundred-megabyte regions are asset pools.
        // Report::bytesSkippedLarge says how much this threw away, so a miss can
        // be told apart from a bad threshold.
        size_t maxRegionBytes = 256ull * 1024 * 1024;

        // Wall clock ceiling. Time is what the player notices, not bytes.
        uint64_t timeBudgetMs = 4000;

        size_t maxHits = 64;
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
        size_t rawMatches = 0;           // pointer matched
        size_t rejectedNoRoom = 0;       // matched but too near the end of its region
        uint64_t microseconds = 0;
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
