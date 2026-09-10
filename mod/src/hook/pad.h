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

    // True on the frame the chord goes from not-all-held to all-held. A held
    // chord fires once. Buttons are XINPUT_GAMEPAD_* bits.
    bool ChordPressed(uint16_t buttons);

    bool Connected();
}
