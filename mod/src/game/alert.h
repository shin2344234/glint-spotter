#pragma once
#include <cstdint>

// The alert system root: what the game calls to put a message on screen.
//
// A popup saying a glint has been marked is what this is for, so the player
// knows to open the map. The game already has one of those and shows it for
// region changes, item pickups and level ups. Finding the call is the same
// problem the map pin was, and it gets the same answer: swap one slot for a
// thunk that writes down every argument and forwards them all unchanged, play
// for a few minutes, then replay a real call with our own text.
//
// Nothing is created here and nothing is altered on the way through. This
// build only watches.

namespace gs::alert
{
    // Take slot 144 on the alert root's vtable. The vtable must be RTTI
    // verified before this is called.
    bool InstallSpy(uintptr_t alertVtable);
    void RemoveSpy();

    // How many calls have gone past.
    uint64_t Seen();
}
