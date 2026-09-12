#pragma once
#include <cstdint>

// The list the game draws its own map markers from.
//
// Read only in this build. The point is to find out whether the chain in
// re-pinmarker-flow.md is right before anything writes down it, and there is
// a clean way to tell: Seth has placed markers by hand, so the list should
// come back holding them, at coordinates that match where he put them. A
// wrong offset gives a count of nine million and floats that are not
// positions, which is its own answer.
//
// If it reads, the mod can stop drawing icons nobody owns and put its pins in
// here instead, where the game's own delete can reach them.

namespace gs::pinmodel
{
    // The submodule off the player, or zero.
    uintptr_t Submodule();

    // Print the marker list for kind 0x15, and enough of its neighbourhood to
    // spot the right offset if 0xC8 is wrong. Safe to call from the tick.
    void LogState(const char* why);

    // Hunt for the list by the coordinates it must contain.
    //
    // Session eighty found the submodule and it is real, a
    // ClientSelfContentsMiscActorComponent, but no list inside it holds
    // markers. Guessing further at offsets is the wrong game when the answer
    // is already written down: the spy captured the game building Seth's own
    // markers from its model, so their coordinates are known. A float that
    // matches one of them, anywhere in that component or in anything it points
    // at, is the list, and the bytes around it are the record layout.
    void HuntByCoordinates(const char* why);
}
