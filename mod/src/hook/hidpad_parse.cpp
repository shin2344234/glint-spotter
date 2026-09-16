#include "hook/hidpad.h"

#include <cstring>

// The report layouts, as SDL's HIDAPI drivers and the Linux hid-sony and
// hid-playstation drivers read them. Both families lay their three button bytes
// out the same way and differ only in where those bytes sit:
//
//   buttons0  low nibble the d-pad hat, 0 up then clockwise, 8 released;
//             0x10 Square, 0x20 Cross, 0x40 Circle, 0x80 Triangle
//   buttons1  0x01 L1, 0x02 R1, 0x04 L2, 0x08 R2, 0x10 Share or Create,
//             0x20 Options, 0x40 L3, 0x80 R3
//   buttons2  0x01 PS, 0x02 touchpad click
//
// DualShock 4, report 0x01 over USB, and the reduced 0x01 a Bluetooth pad sends
// before anything asks it for more: buttons0 at byte 5.
// DualShock 4, report 0x11 over Bluetooth: the same block two bytes later, at 7.
// DualSense, report 0x01 over USB: sticks, triggers and a counter come first,
// so buttons0 is at byte 8.
// DualSense, report 0x31 over Bluetooth: one byte later again, at 9.
// DualSense, report 0x01 over Bluetooth: the simple report a pad sends until
// something enables the full one, laid out like the DualShock's, at byte 5.
//
// Windows pads every HID read to the device's longest report, so the length
// cannot tell the DualSense's two kinds of 0x01 apart. The transport can.
namespace
{
    constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008;
    constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kLS = 0x0040, kRS = 0x0080;
    constexpr uint16_t kLB = 0x0100, kRB = 0x0200;
    constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

    uint16_t FromBlock(uint8_t b0, uint8_t b1)
    {
        static constexpr uint16_t hat[8] = {
            kUp, kUp | kRight, kRight, kDown | kRight, kDown, kDown | kLeft, kLeft, kUp | kLeft,
        };
        uint16_t out = 0;
        const uint8_t h = b0 & 0x0F;
        if (h < 8) out |= hat[h];
        if (b0 & 0x10) out |= kX;
        if (b0 & 0x20) out |= kA;
        if (b0 & 0x40) out |= kB;
        if (b0 & 0x80) out |= kY;
        if (b1 & 0x01) out |= kLB;
        if (b1 & 0x02) out |= kRB;
        if (b1 & 0x10) out |= kBack;
        if (b1 & 0x20) out |= kStart;
        if (b1 & 0x40) out |= kLS;
        if (b1 & 0x80) out |= kRS;
        // L2, R2, PS and the touchpad have no XInput button to become. The
        // triggers are analogue on an Xbox pad and live outside wButtons, and
        // the chord names never included them.
        return out;
    }
}

namespace gs::hidpad
{
    bool ParseInput(Family family, bool bluetooth, const uint8_t* r, size_t length, uint16_t* buttons)
    {
        if (!r || !buttons || length < 1) return false;
        size_t at = 0;
        if (family == Family::DualShock4)
        {
            if (r[0] == 0x01) at = 5;
            else if (r[0] == 0x11) at = 7;
            else return false;
        }
        else
        {
            if (r[0] == 0x01) at = bluetooth ? 5 : 8;
            else if (r[0] == 0x31) at = 9;
            else return false;
        }
        if (length < at + 2) return false;
        *buttons = FromBlock(r[at], r[at + 1]);
        return true;
    }

    size_t BuildRumble(Family family, bool bluetooth, uint8_t strength, uint8_t* out, size_t capacity)
    {
        if (bluetooth || !out) return 0;
        if (family == Family::DualSense)
        {
            // Report 0x02, 48 bytes. Two flags in the first byte say the motor
            // fields are meant, the compatible-vibration one and the one that
            // selects haptics; every other flag left at zero tells the pad to
            // keep its light bar, player lights and triggers as they are, which
            // matters because the game drives those itself.
            constexpr size_t kLen = 48;
            if (capacity < kLen) return 0;
            memset(out, 0, kLen);
            out[0] = 0x02;
            out[1] = 0x01 | 0x02;
            out[3] = strength;   // right, the light motor
            out[4] = strength;   // left, the heavy one
            return kLen;
        }
        // DualShock 4, report 0x05, 32 bytes. The flags byte has one bit each
        // for the motors, the light bar and its flashing, and only the motor
        // bit is set, so the light bar the game chose stays.
        constexpr size_t kLen = 32;
        if (capacity < kLen) return 0;
        memset(out, 0, kLen);
        out[0] = 0x05;
        out[1] = 0x01;
        out[4] = strength;   // right, the light motor
        out[5] = strength;   // left, the heavy one
        return kLen;
    }
}
