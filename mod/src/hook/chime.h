#pragma once

// A sound when a pin lands, for when the pad's buzz is not enough or there is
// no pad. Off unless the ini says Sound=1.
//
// The game's own notice and its own sounds would be better, and neither is
// reachable yet: the alert root that names "Toast" never fired in seven
// minutes of play, and its argument list has never been seen. So this is
// Windows playing a file, through winmm, on the polling thread. The game
// thread only sets a flag, because the first PlaySound call loads the audio
// stack and that is not a wait to put inside a frame.

namespace gs::chime
{
    // Ask for the sound. Safe from any thread; Pump plays it.
    void Ring();

    // Play a sound that was asked for. Called from the polling loop.
    void Pump();
}
