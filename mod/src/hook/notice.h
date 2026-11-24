#pragma once

// A line of text on screen when a pin lands, for a keyboard player who has no
// buzz to tell them. Off unless the ini says Notice=1.
//
// The game's own popup is still out of reach: the alert root's slot names
// "Toast", but the spy on it has never seen the game make a call, so its
// argument list has never been seen filled in and building one by guess
// inside the game's UI is not safe. So this is a small window of the mod's
// own, drawn with GDI on a thread of its own, sitting over the game window
// for two seconds. It never takes focus and clicks pass through it. It shows
// over borderless and windowed play; an exclusive fullscreen swap chain may
// draw over it.
//
// The game thread only sets a flag. Pump, on the key thread, hands the
// request to the notice thread, which owns the window and its message loop.

namespace gs::notice
{
    // Ask for the notice. Safe from any thread.
    void Show();

    // Pass a request on to the notice thread, starting it the first time.
    // Called from the polling loop.
    void Pump();

    // Close the window and let the thread end. On process teardown it does
    // nothing, since the window goes with the process.
    void Stop(bool processTerminating);
}
