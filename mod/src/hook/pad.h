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
    bool ChordHeld(uint16_t buttons, uint32_t holdMs);

    bool Connected();
}
