// PlayStation pad reports into XInput button bits.
//
// There was no DualSense or DualShock 4 on the machine this was written on, so
// these are the only check the parser has had before a player tries it. Each
// report is built byte by byte from the layouts SDL's HIDAPI drivers and the
// Linux hid-sony and hid-playstation drivers read, one per report the two
// families send, and the default chord of LB, RB and A has to come out of every
// one of them as L1, R1 and Cross.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hook/hidpad_parse.cpp"

namespace
{
    using gs::hidpad::Family;

    int failures = 0;

    void Expect(const char* what, bool ok)
    {
        printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++failures;
    }

    constexpr uint16_t xUp = 0x0001, xDown = 0x0002, xLeft = 0x0004, xRight = 0x0008;
    constexpr uint16_t xStart = 0x0010, xBack = 0x0020, xLS = 0x0040, xRS = 0x0080;
    constexpr uint16_t xLB = 0x0100, xRB = 0x0200;
    constexpr uint16_t xA = 0x1000, xB = 0x2000, xX = 0x4000, xY = 0x8000;
    constexpr uint16_t kChord = xLB | xRB | xA;   // Chord=RB+LB+A, the default

    // A report of the length Windows hands back, with the id in front and the
    // two button bytes at `at`. Everything else is left as a pad at rest
    // leaves it: sticks centred at 0x80, the hat released.
    struct Report
    {
        uint8_t b[80];
        size_t len;
        Report(uint8_t id, size_t length, size_t at, uint8_t b0, uint8_t b1)
        {
            memset(b, 0, sizeof(b));
            len = length;
            b[0] = id;
            for (size_t i = 1; i < 5 && i < at; ++i) b[i] = 0x80;
            b[at] = b0;
            b[at + 1] = b1;
            b[at + 2] = 0x01;   // PS held too, which must not become anything
        }
    };

    bool Parse(Family f, bool bt, const Report& r, uint16_t* out)
    {
        return gs::hidpad::ParseInput(f, bt, r.b, r.len, out);
    }

    // Cross down, hat released, L1 and R1 down: the default chord.
    constexpr uint8_t kChordB0 = 0x08 | 0x20;
    constexpr uint8_t kChordB1 = 0x01 | 0x02;
}

int main()
{
    uint16_t got = 0;

    printf("-- the default chord out of every report either family sends --\n");
    Expect("DualShock 4, USB report 0x01",
           Parse(Family::DualShock4, false, Report(0x01, 64, 5, kChordB0, kChordB1), &got) && got == kChord);
    Expect("DualShock 4, Bluetooth reduced report 0x01",
           Parse(Family::DualShock4, true, Report(0x01, 547, 5, kChordB0, kChordB1), &got) && got == kChord);
    Expect("DualShock 4, Bluetooth full report 0x11",
           Parse(Family::DualShock4, true, Report(0x11, 547, 7, kChordB0, kChordB1), &got) && got == kChord);
    Expect("DualSense, USB report 0x01",
           Parse(Family::DualSense, false, Report(0x01, 64, 8, kChordB0, kChordB1), &got) && got == kChord);
    Expect("DualSense, Bluetooth full report 0x31",
           Parse(Family::DualSense, true, Report(0x31, 78, 9, kChordB0, kChordB1), &got) && got == kChord);
    Expect("DualSense, Bluetooth simple report 0x01",
           Parse(Family::DualSense, true, Report(0x01, 78, 5, kChordB0, kChordB1), &got) && got == kChord);

    printf("-- the transport decides where a DualSense 0x01 keeps its buttons --\n");
    // The same bytes read with the wrong transport must not produce the chord:
    // a USB layout read as Bluetooth lands on stick bytes, and the reverse on
    // the trigger and counter bytes.
    Parse(Family::DualSense, true, Report(0x01, 64, 8, kChordB0, kChordB1), &got);
    Expect("a USB 0x01 read as Bluetooth is not the chord", got != kChord);
    Parse(Family::DualSense, false, Report(0x01, 78, 5, kChordB0, kChordB1), &got);
    Expect("a Bluetooth simple 0x01 read as USB is not the chord", got != kChord);

    printf("-- a pad at rest --\n");
    Expect("nothing held reads as nothing",
           Parse(Family::DualSense, false, Report(0x01, 64, 8, 0x08, 0x00), &got) && got == 0);

    printf("-- the face buttons by position, the way Steam Input maps them --\n");
    Parse(Family::DualSense, false, Report(0x01, 64, 8, 0x08 | 0x10, 0), &got);
    Expect("Square is X", got == xX);
    Parse(Family::DualSense, false, Report(0x01, 64, 8, 0x08 | 0x20, 0), &got);
    Expect("Cross is A", got == xA);
    Parse(Family::DualSense, false, Report(0x01, 64, 8, 0x08 | 0x40, 0), &got);
    Expect("Circle is B", got == xB);
    Parse(Family::DualSense, false, Report(0x01, 64, 8, 0x08 | 0x80, 0), &got);
    Expect("Triangle is Y", got == xY);

    printf("-- the second button byte --\n");
    Parse(Family::DualShock4, false, Report(0x01, 64, 5, 0x08, 0x10), &got);
    Expect("Share is BACK", got == xBack);
    Parse(Family::DualShock4, false, Report(0x01, 64, 5, 0x08, 0x20), &got);
    Expect("Options is START", got == xStart);
    Parse(Family::DualShock4, false, Report(0x01, 64, 5, 0x08, 0x40), &got);
    Expect("L3 is LS", got == xLS);
    Parse(Family::DualShock4, false, Report(0x01, 64, 5, 0x08, 0x80), &got);
    Expect("R3 is RS", got == xRS);
    Parse(Family::DualShock4, false, Report(0x01, 64, 5, 0x08, 0x04 | 0x08), &got);
    Expect("L2 and R2 become nothing, having no XInput button", got == 0);

    printf("-- the hat, clockwise from up, and released --\n");
    const uint16_t hat[9] = {xUp, xUp | xRight, xRight, xDown | xRight, xDown,
                             xDown | xLeft, xLeft, xUp | xLeft, 0};
    bool hatOk = true;
    for (uint8_t h = 0; h < 9; ++h)
    {
        Parse(Family::DualSense, true, Report(0x31, 78, 9, h, 0), &got);
        if (got != hat[h]) { printf("      hat %u gave 0x%04X\n", h, got); hatOk = false; }
    }
    Expect("all nine hat values", hatOk);
    Parse(Family::DualSense, true, Report(0x31, 78, 9, 0x0F, 0), &got);
    Expect("an out-of-range hat value is released, not a direction", got == 0);

    printf("-- what is not a button report --\n");
    Expect("a DualSense output report id is refused",
           !Parse(Family::DualSense, false, Report(0x02, 64, 8, kChordB0, kChordB1), &got));
    Expect("a DualShock 4 feature report id is refused",
           !Parse(Family::DualShock4, false, Report(0x05, 64, 5, kChordB0, kChordB1), &got));
    Expect("DualSense 0x11 is a DualShock id and is refused",
           !Parse(Family::DualSense, true, Report(0x11, 78, 7, kChordB0, kChordB1), &got));
    const uint8_t shortUsb[8] = {0x01, 0x80, 0x80, 0x80, 0x80, 0, 0, 0x28};
    Expect("a DualSense USB report too short to reach its buttons is refused",
           !gs::hidpad::ParseInput(Family::DualSense, false, shortUsb, sizeof(shortUsb), &got));
    Expect("an empty read is refused", !gs::hidpad::ParseInput(Family::DualSense, false, shortUsb, 0, &got));
    Expect("a null report is refused", !gs::hidpad::ParseInput(Family::DualSense, false, nullptr, 64, &got));

    printf("-- the buzz --\n");
    uint8_t out[80];
    memset(out, 0xEE, sizeof(out));
    size_t n = gs::hidpad::BuildRumble(Family::DualSense, false, 0x6D, out, sizeof(out));
    bool dsTail = true;
    for (size_t i = 5; i < 48; ++i) if (out[i]) dsTail = false;
    Expect("DualSense USB: report 0x02 of 48 bytes", n == 48 && out[0] == 0x02);
    Expect("DualSense USB: only the two vibration flags are set", out[1] == 0x03 && out[2] == 0x00);
    Expect("DualSense USB: both motors at the strength", out[3] == 0x6D && out[4] == 0x6D);
    Expect("DualSense USB: light bar, player lights and triggers untouched", dsTail);

    memset(out, 0xEE, sizeof(out));
    n = gs::hidpad::BuildRumble(Family::DualShock4, false, 0x2B, out, sizeof(out));
    bool ds4Rest = out[2] == 0 && out[3] == 0;
    for (size_t i = 6; i < 32; ++i) if (out[i]) ds4Rest = false;
    Expect("DualShock 4 USB: report 0x05 of 32 bytes", n == 32 && out[0] == 0x05);
    Expect("DualShock 4 USB: only the motor flag is set, so the light bar stays", out[1] == 0x01);
    Expect("DualShock 4 USB: both motors at the strength", out[4] == 0x2B && out[5] == 0x2B);
    Expect("DualShock 4 USB: every other byte zero", ds4Rest);

    Expect("zero strength is a stop, not a missing report",
           gs::hidpad::BuildRumble(Family::DualSense, false, 0, out, sizeof(out)) == 48 && out[3] == 0);
    Expect("no buzz over Bluetooth, either family",
           gs::hidpad::BuildRumble(Family::DualSense, true, 0x6D, out, sizeof(out)) == 0 &&
           gs::hidpad::BuildRumble(Family::DualShock4, true, 0x6D, out, sizeof(out)) == 0);
    Expect("a buffer too small to hold the report is refused",
           gs::hidpad::BuildRumble(Family::DualSense, false, 0x6D, out, 20) == 0);

    printf(failures ? "\n%d FAILED\n" : "\nall pass\n", failures);
    return failures ? 1 : 0;
}
