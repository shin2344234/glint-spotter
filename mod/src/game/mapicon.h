#pragma once
#include <cstdint>

// The map icon create call, slot 170 on the world map and minimap root controls.
//
// This build spies. It replaces slot 170 in both vtables with a function that
// records every argument the game passes and then forwards all fourteen of them
// to the original, unchanged. Nothing is created by us, nothing is altered on
// the way through. Once a session has captured what the game itself passes, the
// next build can replay a real call with the position swapped, which is a far
// safer first call than filling fourteen arguments from static analysis alone.
//
// What is known about the arguments, from FEASIBILITY.md:
//   1  this, the root control
//   2  const uint16_t*  icon type; indexes a table, wrong values are the crash risk
//   3  const Key*       qword + byte, used by the duplicate lookup
//   4  const uint32_t*
//   5  const Key*       qword + byte, stored in the created object
//   6  const uint32_t*  stored in the created object
//   7  const float*
//   8  const float3*    the position
//   9  const char*      may be null
//  10  const char*      icon name, must be non-empty or the call does nothing
//  11  uint8_t          by value
//  12  const Struct36*  count at +4, pointer at +0x10; zeroed is safe
//  13  uint8_t          by value
// Anything after 13 is forwarded for safety and never read.

namespace gs::mapicon
{
    // A flat copy of one observed call. Plain data on purpose, so it can be
    // filled inside a __try frame.
    struct Capture
    {
        int surface = -1;          // 0 world map, 1 minimap
        uint64_t sequence = 0;
        void* self = nullptr;
        void* raw[14]{};
        uint16_t type = 0;
        uint64_t key3q = 0;  uint8_t key3b = 0;
        uint32_t dword4 = 0;
        uint64_t key5q = 0;  uint8_t key5b = 0;
        uint32_t dword6 = 0;
        float float7 = 0;
        float pos[3]{};
        char str9[48]{};
        char name10[48]{};
        uint8_t byte11 = 0;
        uint8_t struct12[36]{};
        uint8_t byte13 = 0;
        bool str9Null = true;
        bool ok = false;          // every read succeeded
    };

    // Swap slot 170 on both vtables. Both must be RTTI-verified before this is
    // called. Returns how many of the two were installed.
    int InstallSpy(uintptr_t worldVtable, uintptr_t miniVtable);
    void RemoveSpy();

    // Counts and the most recent capture per surface, for the hotkey dry run.
    uint64_t Seen(int surface);
    bool Last(int surface, Capture& out);
}
