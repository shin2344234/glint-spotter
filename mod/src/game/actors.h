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
        bool knowledge = false;      // carries a ClientKnowledgeActorComponent
        bool locked = false;
        bool shown = false;          // scratch, for the one-shot set listing
        const char* how = "";        // which frame answered for its position
        char name[48]{};             // the gimmick's node name, when it has one
        uintptr_t gimmickComp = 0;
        uintptr_t detectComp = 0;    // its own ClientDetectActorComponent, or 0
        uintptr_t effectComp = 0;    // its ClientEffectActorComponent, or 0
        // What each component's first qword read when it was found. A load
        // frees these blocks and the game puts something else in them, so a
        // byte read out of one afterwards is whatever moved in. Checked on
        // every read, the way the flash flag has been checked since 1.1.21.
        uintptr_t gimmickVt = 0;
        uintptr_t detectVt = 0;
    };

    // Give the finder the manager's vtable, from the RTTI sweep. It looks for
    // the globals itself.
    void SetManagerVtable(uintptr_t vtable);

    // The gimmick component's vtable, RTTI-verified, so slot +0x30 of a block
    // can be checked with one compare. Optional: without it the slots are
    // named through RTTI, which is slower.
    void SetGimmickVtable(uintptr_t vtable);

    // Throw the set away. Everything in it points into blocks the game is
    // about to free, and an entry that outlives its object is read as
    // whatever lands in that memory next.
    void Forget(const char* why);

    // Try to locate the live manager. Cheap when already found.
    bool Locate(uint32_t nowMs);
    bool Ready();
    uintptr_t Manager();

    // Read every pool once and merge into the set. Call from a thread that is
    // not the game's. Returns how many entities the pools held this call.
    uint32_t Refresh(uint32_t nowMs);

    // The manager's pools as they stand, entity pointers and nothing else.
    //
    // The set below is the useful view and it is also a filtered one: an entry
    // only survives if its world position can be worked out, and that needs
    // the player's position for the sub-level origin. So the set is empty
    // exactly when the player is missing, which is exactly when something
    // wants to go looking for him. This is the unfiltered read for that case.
    // Returns how many pointers were written.
    int Offered(uintptr_t* out, int n);

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

    // The marked nodes within `radius` of a point, nearest first. Distance
    // is measured flat: the heights disagree with the player's by several
    // metres and nothing should turn on them.
    int MarkedNear(float px, float pz, float radius, Entity* out, int n);

    // The marked nodes the crosshair is on, smallest bearing error first, so
    // out[0] is the pick. The whole set is measured, not a nearest handful:
    // session fifty asked for the nearest sixteen and then chose among those,
    // and a berry bush three metres away won over the glint the player was
    // aiming at, which is why the pin landed short.
    //
    // `px, pz` is the player, `ox, oz` the eye, `ux, uz` the unit view bearing.
    // Nodes closer than `minFromPlayer` to the player cannot be picked, and
    // nodes whose bearing error exceeds `maxAngle` radians are not candidates.
    // `angles` receives each kept node's bearing error in radians. `marked`,
    // when given, receives how many marked nodes were in radius at all.
    int MarkedOnBearing(float px, float pz, float ox, float oz, float ux, float uz,
                        float radius, float maxAngle, float minFromPlayer,
                        Entity* out, float* angles, int n, int* marked);

    // Every entity in the set, by how far off the given bearing it sits, with
    // its class, its distance, and what the mod makes of it.
    //
    // Session fifty-five is why it is every entity and not only the gimmicks.
    // I marked the glint on the map: a hundred and nineteen metres north of
    // me, the same one session forty-seven measured at a hundred and
    // eighteen. The gimmick listing for that press reached seventy degrees off
    // the crosshair and out to two hundred and sixty metres and held nothing
    // between ninety and a hundred and fifty. The set had two hundred and
    // fifty entities and eighty-eight of them were gimmicks, so the glint is
    // either one of the other hundred and sixty-two or it is not in the pools
    // at all. Those need different answers and this says which.
    void LogEntities(float px, float pz, float ox, float oz, float ux, float uz);

    // How far the farthest marked node in the set stands from a point, flat.
    // The log says it on every press so that a pin landing short can be told
    // apart from a glint the game never put in the pools at all.
    float MarkedReach(float px, float pz);

    // The entities whose gimmick component carries the detect mode target
    // byte, nearest the given bearing first, with their bearing errors.
    //
    // Session fifty-seven is the first time in the project that byte ever
    // moved: "glint byte set on eid B0100301 at (-9706.3, 566.7, -4162.5)"
    // while I was walking toward the glint I had marked. Every earlier
    // session read it on every gimmick every pass and never saw it change,
    // which is explained now: the object it belongs to is not in the pools
    // until the player is near it, so there was nothing to watch.
    int GlintOnBearing(float px, float pz, float ox, float oz, float ux, float uz,
                       Entity* out, float* angles, int n);

    // Is this pointer one of the entities the manager's pools hold?
    //
    // The test that tells a real actor from heap that happens to look like
    // one. FindDetectTargetTask is a fixed 112-byte allocation, proved from
    // the literal size at its only allocation site, so session nineteen's
    // read at +0x540 was more than a kilobyte past the end of the object and
    // whatever it found was the neighbouring allocation. The pools are the
    // authority on what is a live entity, and the mod already walks them four
    // times a second.
    bool InSet(uintptr_t ptr);

    // The entity carrying this id, from the set, or 0.
    uintptr_t ByEid(uint32_t eid);

    // The played character's own entity, from the pools: the one whose id
    // begins 0xA0 and whose block carries a special mode component. The
    // manager hands it over like any other, so there is no reason to scan
    // the heap for the player. Zero until a world is loaded.
    uintptr_t PlayerEntity(uintptr_t* specialComponent);
}
