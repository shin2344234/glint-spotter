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
        float x = 0, y = 0, z = 0;
        bool valid = false;
    };

    // Tell the reader where the player's special mode component is. Comes from
    // the scan; cleared to null if that object stops carrying its vtable.
    void SetSpecialComponent(void* comp);

    // Walk component -> actor -> transform and read the position. Every read is
    // guarded. Logs what it finds on the first few calls so a wrong assumption
    // about the layout is visible in the log rather than silently wrong.
    Pos Read();

    // The most recent valid read, for callers on other threads.
    Pos Last();
}
