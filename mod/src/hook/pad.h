#pragma once
#include <cstdint>

// Controller chords through XInput, polled, never hooked.
//
// XInputGetState is a read: any number of callers can poll it and none of them
// takes the input away from the game. The library is loaded by name at runtime
// so a machine without it just reports no controller rather than failing to
// load the plugin.
//
// All four XInput slots are watched, not slot 0 alone. A pad rarely lands on a
// slot the mod picked for it: Steam Input and DS4Windows both present a virtual
// pad whose index depends on what else is plugged in, and Erinion's DualSense
// report is what this cost. The first slot to answer is kept and asked on its
// own after that, and the hunt across all four runs once a second while none
// answers, because asking an empty slot is the slow call in this API.
//
// A DualSense or DualShock 4 with nothing translating it does not appear on
// XInput at all. hidpad.cpp reads those directly and ChordHeld joins its buttons
// to XInput's, so a chord works on either kind of pad and on both at once.

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
