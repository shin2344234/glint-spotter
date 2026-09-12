#pragma once
#include <cstdint>

// The player's live world position, read off the actor.
//
// The minimap control turned out not to hold it inline: twenty five samples of
// walking and turning moved no float in the control at all. The actor does hold
// it, and Master Looter's offsets say where: the entity's component block is at
// entity+0x68, the transform component pointer sits at block+0x1A0, and the
// transform carries a parent-relative position at +0xB4 with the parent's world
// position at +0xEC when the parent id at +0xC8 is set.
//
// The way in is the player's special mode component, which the scan finds by
// RTTI, and whose +0x08 should be the owning actor. That "should" is what the
// first session with this file tests: it logs the RTTI name of what +0x08 points
// at, and of every component in the block, before trusting any of it.

namespace gs::player
{
    struct Pos
    {
        float x = 0, y = 0, z = 0;       // world, what the map draws
        float lx = 0, ly = 0, lz = 0;    // local to the sub-level, what the transform's +0xB4 holds
        float ox = 0, oy = 0, oz = 0;    // world minus local: the sub-level origin
        float q[4]{};                    // facing, yaw quaternion at transform +0x28C
        bool valid = false;
        // True when this came from the camera rather than from the player's
        // own component. The position is as good; what is missing is the
        // flash, so the automatic glint marker still waits and a press does
        // not have to.
        bool fromCamera = false;
    };

    // Tell the reader where the player's special mode component is. Comes from
    // the scan; cleared to null if that object stops carrying its vtable.
    void SetSpecialComponent(void* comp);

    // Find the player again by asking the actor manager rather than the heap.
    //
    // A load throws away everything the mod holds and the sweep that finds it
    // again reads gigabytes. The manager's pools hold the new player straight
    // away, and he names himself: his component block carries a
    // ClientUserLoginActorComponent and nothing else in the world does.
    //
    // This recovers the player, which is the position, the marker list, the
    // pins and marking by hand. It does not recover the special mode
    // component, which is not on him and is what the flash needs; that still
    // waits for the sweep. Call from a thread that is not the game's.
    bool Recover();

    // Walk component -> actor -> transform and read the position. Every read is
    // guarded. Logs what it finds on the first few calls so a wrong assumption
    // about the layout is visible in the log rather than silently wrong.
    Pos Read();

    // The most recent valid read, for callers on other threads.
    Pos Last();

    // The actor the walk resolved and the detect component in its block, both
    // zero until the first successful read.
    uintptr_t Actor();
    uintptr_t DetectComponent();
    uintptr_t CharacterControlComponent();

    // True when the last successful walk found the owner to be the played
    // body, ClientChildOnlyInGameActor. Every character carries a special
    // mode component; only the player's owner is that class.
    bool OwnerIsPlayedBody();
}
