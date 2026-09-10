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
// The arguments. Eleven of them; the first spy session read thirteen because
// the dispatcher pushes seven registers where the constructor pushes five and
// the constructor's stack offsets were reused. Arguments 12 to 14 came back as
// return addresses, which is how that was caught.
//   1  this, the root control
//   2  const uint16_t*  icon type; 0 for the player, 0x020F for map points
//   3  const Key*       int64 id + uint8 kind; A0100001/0C is the player actor
//   4  const uint32_t*  the same id again for the player, 0 for map points
//   5  const float*
//   6  const float3*    the position
//   7  const char*      may be null
//   8  const char*      icon name, must be non-empty or the call does nothing
//   9  uint8_t          by value
//  10  const Struct36*  count at +4, pointer at +0x10; zeroed is safe
//  11  uint8_t          by value
// Three more are forwarded for safety and never read.

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
        int64_t keyId = 0;  uint8_t keyKind = 0;
        uint32_t dword4 = 0;
        float float5 = 0;
        float pos[3]{};
        char str7[48]{};
        char name8[64]{};
        uint8_t byte9 = 0;
        uint8_t struct10[36]{};
        uint8_t byte11 = 0;
        bool str7Null = true;
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
