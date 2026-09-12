#pragma once
#include <cstdint>

// Snapshot the player's whole component block on a press and diff it against
// the press before, on the game's thread, so the aim target reveals itself in
// one session rather than across three.
//
// Session sixteen showed the flash finds its target by a physics query and
// records it as something other than an actor pointer. Which component holds
// the record is not known, so all of them are copied: every component in the
// played body's block, the actor itself, and the detect task. Press one is the
// baseline with the flash on and nothing aimed at; press two is aimed at a
// glint. Every dword that changed is logged, and two shapes are acted on: a
// dword that now looks like an actor id, resolved through the actor manager,
// and a float triple that now sits within eighty units of the player.

namespace gs::snapshot
{
    struct Found
    {
        bool valid = false;
        float x = 0, y = 0, z = 0;
        char how[96]{};     // where it came from, for the log and the write-up
    };

    // Take a snapshot of everything reachable from the player's actor. Logs the
    // full hex of each object on the first two presses. If a previous snapshot
    // exists with the same objects, diffs against it and returns anything that
    // looks like the aim target. Must be called on the game's thread.
    Found PressAndDiff(uintptr_t playerActor, uintptr_t detectTask,
                       float px, float py, float pz);
}
