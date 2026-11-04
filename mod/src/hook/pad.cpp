#include "hook/pad.h"

#include <Windows.h>
#include <Xinput.h>

#include <atomic>

#include "core/log.h"
#include "core/settings.h"
#include "hook/hidpad.h"

namespace
{
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    GetStateFn g_getState = nullptr;
    constexpr int kChords = 2;
    uint32_t g_sinceMs[kChords]{};   // when each chord went down, or zero
    bool g_fired[kChords]{};         // this hold has already had its turn
    bool g_connected = false;
    int g_pad = -1;                  // the XInput slot answering, -1 while none is
    int g_loggedPad = -2;            // the slot the log last named, -2 before it named any
    uint32_t g_lastHuntMs = 0;       // when the hunt across all four slots last ran
    constexpr uint32_t kHuntEveryMs = 1000;
    // A buzz is a run of equal pulses: on for g_buzzOn, off for g_buzzOff,
    // g_buzzCount times, from g_buzzStart. A single buzz is a run of one.
    // g_buzzStart is written last, so Pump never pairs a new start with an
    // old shape for longer than one of its own passes.
    std::atomic<uint32_t> g_buzzStart{0};
    std::atomic<uint32_t> g_buzzOn{0};
    std::atomic<uint32_t> g_buzzOff{0};
    std::atomic<uint32_t> g_buzzCount{0};
    std::atomic<uint16_t> g_buzzStrength{0};
    bool g_buzzing = false;
    uint16_t g_sentStrength = 0;   // what the motors were last told, 0 while off
    // Pump's own clock for the pattern it is playing. A pattern is timed from
    // the pass that first sees it, not from when Buzz was called: the loop
    // that runs Pump takes a pool walk of 40 to 50 ms every half second on top
    // of its 50 ms sleep, so a 90 ms buzz timed from the call could be over
    // before Pump ever looked and never reach the motors at all.
    uint32_t g_playing = 0;        // the g_buzzStart value being played, 0 for none
    uint32_t g_playFrom = 0;       // when Pump first saw it
    uint32_t g_lastSendMs = 0;     // when the motors were last written
    bool g_sendFailSaid = false;   // a failed write has been logged this session
    // While a buzz is on it is written again every pass. The game calls
    // XInputSetState itself, through a wrapper at 0xEDBA0F0 on 1.0.0.3527 that
    // scales both motors by its own vibration setting, so a write of its own
    // can stop ours. AsunaKaoru felt no buzz at all on 1.1.39-test1, with every
    // press handled and the pad on XInput slot 0.
    constexpr uint32_t kResendMs = 40;
    using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
    SetStateFn g_setState = nullptr;

    // Look for a pad on any of the four slots and keep the first that answers.
    //
    // Asking a slot with nothing on it is the expensive call in XInput, so this
    // runs once a second rather than on every poll. Twenty polls a second
    // against four empty slots is eighty of the slow ones, for a machine that
    // has no controller at all and never will.
    bool HuntForPad(uint32_t now, XINPUT_STATE& st)
    {
        if (g_lastHuntMs && now - g_lastHuntMs < kHuntEveryMs) return false;
        g_lastHuntMs = now ? now : 1;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
        {
            XINPUT_STATE probe{};
            if (g_getState(i, &probe) != ERROR_SUCCESS) continue;
            g_pad = static_cast<int>(i);
            st = probe;
            return true;
        }
        return false;
    }
}

namespace gs::pad
{
    bool Init()
    {
        const wchar_t* names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        for (const wchar_t* n : names)
        {
            HMODULE h = LoadLibraryW(n);
            if (!h) continue;
            g_getState = reinterpret_cast<GetStateFn>(GetProcAddress(h, "XInputGetState"));
            g_setState = reinterpret_cast<SetStateFn>(GetProcAddress(h, "XInputSetState"));
            if (g_getState)
            {
                GS_LOG("pad: using %ls, vibration %s", n, g_setState ? "available" : "not available");
                break;
            }
        }
        if (!g_getState) GS_LOG("pad: no XInput library, so Xbox pads are not read");
        if (gs::Settings::Get().directPad) gs::hidpad::Start();
        else GS_LOG("pad: DirectPad=0, so a PlayStation pad works only through Steam Input or DS4Windows");
        return g_getState != nullptr || gs::Settings::Get().directPad;
    }

    bool ChordHeld(uint16_t buttons, uint32_t holdMs, int slot)
    {
        if (!buttons) return false;
        if (slot < 0 || slot >= kChords) return false;

        const uint32_t now = GetTickCount();

        // Slot 0 used to be the only one asked. A pad that enumerates anywhere
        // else, which is what a virtual pad in front of a DualSense usually
        // does, was a pad the mod could not see at all.
        XINPUT_STATE st{};
        bool answered = false;
        if (g_getState)
        {
            if (g_pad >= 0)
            {
                answered = g_getState(static_cast<DWORD>(g_pad), &st) == ERROR_SUCCESS;
                if (!answered) g_pad = -1;   // let it go and hunt again, it may come back elsewhere
            }
            if (!answered) answered = HuntForPad(now, st);
        }

        if (g_pad != g_loggedPad)
        {
            if (g_pad >= 0)
                GS_LOG("pad: controller on XInput slot %d", g_pad);
            else if (g_loggedPad >= 0)
                GS_LOG("pad: the controller on slot %d is gone", g_loggedPad);
            else
                GS_LOG("pad: nothing on any of the four XInput slots. A DualSense or DualShock 4 "
                       "is read directly instead and says so when it is found.");
            g_loggedPad = g_pad;
        }

        // Both at once when Steam Input is on: its virtual Xbox pad answers on
        // XInput and the real pad underneath still reports over HID. The masks
        // are joined before the chord is judged, so one press cannot fire twice.
        const bool direct = gs::hidpad::Connected();
        const uint16_t pressed = static_cast<uint16_t>((answered ? st.Gamepad.wButtons : 0) |
                                                       (direct ? gs::hidpad::Buttons() : 0));
        const bool was = g_connected;
        g_connected = answered || direct;
        if (!g_connected)
        {
            g_sinceMs[slot] = 0;
            g_fired[slot] = false;
            // The pad left mid-buzz. Forget that the motors were running, or
            // the next Pump after it comes back sees the state it wanted and
            // makes no call.
            if (was) { g_buzzing = false; g_sentStrength = 0; g_lastSendMs = 0; }
            return false;
        }

        const bool held = (pressed & buttons) == buttons;
        if (!held) { g_sinceMs[slot] = 0; g_fired[slot] = false; return false; }

        if (!g_sinceMs[slot]) g_sinceMs[slot] = now ? now : 1;
        if (g_fired[slot]) return false;
        if (now - g_sinceMs[slot] < holdMs) return false;
        g_fired[slot] = true;
        return true;
    }

    void Buzz(uint16_t strength, uint32_t ms) { Pulses(strength, ms, 0, 1); }

    void Pulses(uint16_t strength, uint32_t onMs, uint32_t offMs, uint32_t count)
    {
        const uint32_t now = GetTickCount();
        // A weaker buzz waits for a stronger one to finish. AsunaKaoru pressed
        // again within 150 ms of a pin landing on 4 November, the second press
        // found the pin already there, and its 90 ms nothing-placed buzz
        // restarted the clock on the 450 ms pin buzz and cut it to 90.
        if (const uint32_t start = g_buzzStart.load())
        {
            const uint32_t period = g_buzzOn.load() + g_buzzOff.load();
            const uint32_t total = period * g_buzzCount.load();
            if (strength < g_buzzStrength.load() && now - start < total) return;
        }
        g_buzzStrength.store(strength);
        g_buzzOn.store(onMs);
        g_buzzOff.store(offMs);
        g_buzzCount.store(count);
        g_buzzStart.store(now ? now : 1);
    }

    void Pump()
    {
        // XInput first when there is a pad on it. With Steam Input on, that is
        // the virtual pad, and Steam passes the buzz on to the real one; writing
        // to the real one as well would buzz it twice over.
        const bool viaXInput = g_setState && g_pad >= 0;
        const bool viaDirect = !viaXInput && gs::hidpad::CanRumble();
        if (!viaXInput && !viaDirect) return;
        const uint32_t now = GetTickCount();
        bool want = false;
        bool fresh = false;
        const uint32_t start = g_buzzStart.load();
        if (start && start != g_playing)
        {
            g_playing = start;
            g_playFrom = now;
            fresh = true;
        }
        if (g_playing)
        {
            const uint32_t on = g_buzzOn.load();
            const uint32_t period = on + g_buzzOff.load();
            const uint32_t elapsed = now - g_playFrom;
            want = period != 0 && elapsed / period < g_buzzCount.load() && elapsed % period < on;
        }
        // A new pattern can start while the motors run; its strength is sent
        // even though on stays on. While on, it is sent again every pass.
        const uint16_t strength = want ? g_buzzStrength.load() : 0;
        const bool changed = want != g_buzzing || strength != g_sentStrength;
        if (!changed && !(want && now - g_lastSendMs >= kResendMs)) return;
        g_buzzing = want;
        g_sentStrength = strength;
        g_lastSendMs = now ? now : 1;
        bool ok = true;
        DWORD rc = ERROR_SUCCESS;
        if (viaXInput)
        {
            XINPUT_VIBRATION v{};
            v.wLeftMotorSpeed = strength;
            v.wRightMotorSpeed = strength;
            rc = g_setState(static_cast<DWORD>(g_pad), &v);
            ok = rc == ERROR_SUCCESS;
        }
        else
        {
            ok = gs::hidpad::Rumble(want ? static_cast<uint8_t>(strength >> 8) : 0);
        }
        // One line per buzz, so a report of a silent pad can be checked
        // against what was sent and what the call said back.
        if (fresh && want)
        {
            if (viaXInput)
                GS_LOG("pad: buzz %u for %u ms (x%u, %u ms after it was asked for) on XInput slot %d; "
                       "XInputSetState returned %lu",
                       strength, g_buzzOn.load(), g_buzzCount.load(), now - start, g_pad,
                       static_cast<unsigned long>(rc));
            else
                GS_LOG("pad: buzz %u for %u ms (x%u, %u ms after it was asked for) through the pad's "
                       "HID output; the write %s",
                       strength, g_buzzOn.load(), g_buzzCount.load(), now - start,
                       ok ? "went through" : "failed");
        }
        else if (!ok && !g_sendFailSaid)
        {
            g_sendFailSaid = true;
            if (viaXInput)
                GS_LOG("pad: XInputSetState on slot %d returned %lu", g_pad, static_cast<unsigned long>(rc));
            else
                GS_LOG("pad: a rumble write through the pad's HID output failed");
        }
    }

    bool Connected() { return g_connected; }
}
