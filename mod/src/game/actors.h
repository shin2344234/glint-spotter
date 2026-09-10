#pragma once
#include <cstdint>

// The world's entities, the way Master Looter reaches them.
//
// The ClientActorManager is not found by scanning the heap for its vtable: that
// turned up a registry entry in session nineteen and the ray had nothing to
// test. It is found the way Master Looter finds it, through global pointers in
// the exe's own data that point at an object carrying the manager's vtable.
// There are several, and the one holding a world is the live one, so the pick
// is by how many entities its pools hold, rechecked once the world has loaded.
//
// The entities sit in several pools between +0x100 and +0x200 of the manager,
// each a count, a capacity and a pointer to an array of entity pointers, and
// the manager hands the world over a few at a time. So callers do not read the
// pools once; the tick feeds them into a set that keeps each entity for twelve
// seconds after it was last seen, and the ray casts against the set.

namespace gs::actors
{
    struct Entity
    {
        uintptr_t ptr = 0;
        uint32_t eid = 0;
        float x = 0, y = 0, z = 0;   // world, from the transform at +0x29C
        uint32_t lastSeenMs = 0;
    };

    // Give the finder the manager's vtable, from the RTTI sweep. It looks for
    // the globals itself.
    void SetManagerVtable(uintptr_t vtable);

    // Try to locate the live manager. Cheap when already found; recheck is
    // forced once, twenty seconds after the first find, when the world exists.
    // Returns true when a manager with entities is in hand.
    bool Locate(uint32_t nowMs);
    bool Ready();
    uintptr_t Manager();

    // Read every pool once and merge into the set. Call from the tick, a few
    // times a second. Returns how many entities the pools held this call.
    uint32_t Refresh(uint32_t nowMs);

    // The accumulated set. Entries older than twelve seconds are dropped on
    // Refresh. `out` receives up to n entries; returns how many.
    int Snapshot(Entity* out, int n);
    int Count();

    // The entity carrying this id, from the set, or 0.
    uintptr_t ByEid(uint32_t eid);
}
