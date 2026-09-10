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
// One walk covers every pointer value you care about. Walking once per class was
// the first version and it cost a second pass over gigabytes for no reason, so
// pass all the needles together.

namespace gs::scan
{
    struct Hit
    {
        void* object = nullptr;   // the candidate, vtable pointer at offset 0
        int needle = -1;          // which entry of the needles array matched
        uintptr_t regionBase = 0;
        size_t regionSize = 0;
        uint32_t regionType = 0;  // MEM_PRIVATE or MEM_MAPPED
    };

    struct Report
    {
        size_t regionsSeen = 0;
        size_t regionsScanned = 0;
        uint64_t bytesScanned = 0;
        uint64_t microseconds = 0;
        bool budgetHit = false;   // stopped early on the byte budget
    };

    // Every committed, readable, non-image location holding one of these exact
    // pointer values. Stops after maxHits candidates in total, or once the byte
    // budget is spent, so a bad vtable value cannot turn into a long stall.
    Report FindPointers(const uintptr_t* needles, size_t needleCount,
                        std::vector<Hit>& out,
                        size_t maxHits = 64,
                        uint64_t byteBudget = 6ull * 1024 * 1024 * 1024);

    // Cheap re-check of a pointer a previous scan returned: is it still readable
    // and does it still carry this vtable. Microseconds, so it is what the
    // steady state should call instead of scanning again.
    bool StillValid(const void* object, uintptr_t vtableAddress);
}
