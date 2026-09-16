#pragma once
#include <cstddef>
#include <cstdint>

// PlayStation pads read straight off HID, for when nothing has turned them into
// an Xbox pad.
//
// XInput only ever sees Xbox pads and virtual ones. A DualSense or DualShock 4
// on its own is a HID device, and the only way the chord used to work on one
// was Steam Input or DS4Windows putting a virtual Xbox pad in front of it.
// Erinion confirmed that works on 16 September 2026, and also what it costs:
// the game then draws Xbox glyphs, because it is being handed an Xbox pad.
// Reading the pad directly lets Steam Input stay off.
//
// Reading is not exclusive. Every open handle on a HID device gets its own copy
// of each input report, so the game and Steam go on reading theirs untouched.
// The mod never sends the feature requests that switch a Bluetooth pad between
// report modes, because the game may depend on the mode it chose; it parses
// whichever report the pad is already sending.
//
// Buttons come out as XINPUT_GAMEPAD_* bits, so a chord means the same thing on
// either kind of pad: LB is L1, RB is R1, A is Cross, B is Circle, X is Square,
// Y is Triangle, BACK is Share or Create, START is Options.
namespace gs::hidpad
{
    enum class Family : uint8_t
    {
        DualShock4,
        DualSense,   // the Edge speaks the same reports
    };

    // Pure functions, in hidpad_parse.cpp, so the tests can drive them with
    // byte arrays and no hardware.

    // One input report as ReadFile returned it, report id first. False when
    // the report is not one this family sends or is too short to hold buttons.
    bool ParseInput(Family family, bool bluetooth, const uint8_t* report, size_t length,
                    uint16_t* buttons);

    // An output report that sets both motors to `strength` and nothing else.
    // Returns the number of bytes to send, or 0 when this pad cannot be buzzed
    // this way: Bluetooth output needs a checksum and can switch the pad's
    // report mode, and that is not something to do untested from inside the
    // game.
    size_t BuildRumble(Family family, bool bluetooth, uint8_t strength, uint8_t* out,
                       size_t capacity);

    // The device side, in hidpad.cpp. A thread of its own finds a pad, reads it,
    // and looks again every two seconds when there is none.
    void Start();
    void Stop(bool processTerminating);

    bool Connected();
    uint16_t Buttons();      // zero when no report has arrived for a second
    bool CanRumble();
    bool Rumble(uint8_t strength);   // zero stops the motors
}
