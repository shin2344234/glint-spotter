#pragma once
#include <cstdint>

// Turning an actor id into an actor.
//
// This engine keys everything by a 32-bit actor id, A0100001 for the player,
// B0-prefixed for world objects, and the detect component records what the
// flash is aimed at as one of those rather than as a pointer. Master Looter's
// notes give the way back: the one ClientActorManager, found by RTTI, holds an
// entity list at +0x190 as count, capacity and pointer, and every entity keeps
// its id at +0x60. This walks that list.
//
// The manager is found by scanning for its vtable the same way the map roots
// are, once, and rechecked before use.

namespace gs::actors
{
    // Give the lookup the manager, found by the scan.
    void SetManager(void* manager);
    bool Ready();
    void* ManagerPtr();

    // The entity carrying this id, or 0. Walks the list; guarded throughout.
    uintptr_t ByEid(uint32_t eid);

    // How many entities the list held on the last walk, for the log.
    uint32_t LastCount();
}
