#pragma once
#include <cstdint>

// The game's own "picked something up" message, heard as it is handled.
//
// Picking up a sealed artifact changes no state the save reader can see: on
// 23 September the artifact's item gimmick stayed in Wait and the four save
// records on it stayed Wait, even with the player 115 metres away. So the flash
// marked it again, and a pin on it never cleared. What does happen is that the
// game raises TrocTrProcessPickUpItemOnceTimer, the message Master Looter forges
// to loot, with a mode byte at +3 of its payload and the picked-up entity's id
// at +4.
//
// The message is a class, and the game hands it to slot 2 of that class's
// vtable to be handled. On 2976 that is +0x5B34BB8, and slot 2 is a jump into
// the protected code. Its siblings on either side take (this, out, context)
// with the payload at context +0x18, and read the payload the same way the
// documented layout says. So the mod swaps that one slot, writes the id down,
// and forwards to whatever the slot held before, the same way every other hook
// here stacks with another mod's.
//
// Anything picked up counts, the player's or a pet's: either way it has gone.

namespace gs::pickup
{
    // Find the class by RTTI and swap the slot. Safe to call once at startup.
    bool Install();
    void Remove();

    // Entity ids picked up since the last call, oldest first. Any thread.
    int Take(uint32_t* out, int n);

    // How many pickups the hook has seen this session, for the log.
    uint32_t Seen();
}
