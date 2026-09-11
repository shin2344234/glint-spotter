#pragma once
#include <cstdint>

// The world's entities, the way Master Looter reaches them.
//
// The ClientActorManager is found through global pointers in the exe's own
// data that point at an object carrying the manager's vtable. There are
// several, and the one holding a world is the live one, so the pick is by how
// many entities its pools hold, rechecked while it offers none.
//
// The world sits in a dozen pools between +0x100 and +0x300 of the manager,
// each a bare pointer to an array of entity pointers with no usable count.
// The read walks each pool forward while the entries are entities. The key
// thread feeds them into a set that keeps each entity for twelve seconds
// after it was last seen, and the casts run against the set.
//
// Each entity in the set knows whether it carries a gimmick component, and
// each pass reads that component's detect mode target byte: the one the
// EnableDetectMode gimmick event sets, the one that says this object glints
// under the blinding flash.

namespace gs::actors
{
    struct Entity
    {
        uintptr_t ptr = 0;
        uint32_t eid = 0;
        float x = 0, y = 0, z = 0;   // world, from the transform at +0x29C
        uint32_t lastSeenMs = 0;
        bool gimmick = false;        // carries a ClientGimmickActorComponent
        bool glint = false;          // its detect mode target byte is set
        bool lit = false;            // the reveal effect is on it right now
        bool pickup = false;         // a gimmick carrying item or gather data
        bool locked = false;
        bool parented = false;       // its position came from the parent chain
        char name[48]{};             // the gimmick's node name, when it has one
        uintptr_t gimmickComp = 0;
        uintptr_t detectComp = 0;    // its own ClientDetectActorComponent, or 0
        uintptr_t effectComp = 0;    // its ClientEffectActorComponent, or 0
    };

    // Give the finder the manager's vtable, from the RTTI sweep. It looks for
    // the globals itself.
    void SetManagerVtable(uintptr_t vtable);

    // The gimmick component's vtable, RTTI-verified, so slot +0x30 of a block
    // can be checked with one compare. Optional: without it the slots are
    // named through RTTI, which is slower.
    void SetGimmickVtable(uintptr_t vtable);

    // Try to locate the live manager. Cheap when already found.
    bool Locate(uint32_t nowMs);
    bool Ready();
    uintptr_t Manager();

    // Read every pool once and merge into the set. Call from a thread that is
    // not the game's. Returns how many entities the pools held this call.
    uint32_t Refresh(uint32_t nowMs);

    // The accumulated set. Entries older than twelve seconds are dropped on
    // Refresh. `out` receives up to n entries; returns how many.
    int Snapshot(Entity* out, int n);
    int Count();
    int GimmickCount();
    int GlintCount();
    int LitCount();
    int PickupCount();

    // The lit entities, wherever they are, nearest the given point first.
    int LitNear(float px, float py, float pz, Entity* out, int n);

    // The entity carrying this id, from the set, or 0.
    uintptr_t ByEid(uint32_t eid);
}
