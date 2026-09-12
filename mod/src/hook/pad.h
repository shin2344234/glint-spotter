#pragma once
#include <cstdint>

// Controller chords through XInput, polled, never hooked.
//
// XInputGetState is a read: any number of callers can poll it and none of them
// takes the input away from the game. The library is loaded by name at runtime
// so a machine without it just reports no controller rather than failing to
// load the plugin.

namespace gs::pad
{
    bool Init();

    // True once, after the chord has been held unbroken for `holdMs`.
    //
    // The old version fired the instant every button went down, on RB, LB and
    // A, which are three buttons the game itself uses. Any moment those three
    // overlapped during play was a mark nobody asked for. A hold costs the
    // player a third of a second and costs the game nothing, because no combat
    // input lasts that long by accident.
    //
    // Buttons are XINPUT_GAMEPAD_* bits.
    // `slot` picks which chord's press-and-hold state this is, so two
    // chords can be watched without one swallowing the other's edge.
    bool ChordHeld(uint16_t buttons, uint32_t holdMs, int slot = 0);

    // Buzz the pad for a moment. Safe from any thread: it records what it
    // wants and Pump, on the polling thread, makes the XInput call.
    //
    // This is the feedback the mod has been missing. I place a marker and
    // nothing tells me, because a marker on a map I am not looking at is not
    // a notification. The game's own text popups are still unfound. A buzz
    // costs one call and arrives the instant the pin lands.
    void Buzz(uint16_t strength, uint32_t ms);

    // Apply or clear a pending buzz. Called from the polling loop.
    void Pump();

    bool Connected();
}
