#include "hook/pad.h"

#include <Windows.h>
#include <Xinput.h>

#include <atomic>

#include "core/log.h"

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
    std::atomic<uint32_t> g_buzzUntil{0};
    std::atomic<uint16_t> g_buzzStrength{0};
    bool g_buzzing = false;
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
                return true;
            }
        }
        GS_LOG("pad: no XInput library, controller chord off");
        return false;
    }

    bool ChordHeld(uint16_t buttons, uint32_t holdMs, int slot)
    {
        if (!g_getState || !buttons) return false;
        if (slot < 0 || slot >= kChords) return false;

        const uint32_t now = GetTickCount();

        // Slot 0 used to be the only one asked. A pad that enumerates anywhere
        // else, which is what a virtual pad in front of a DualSense usually
        // does, was a pad the mod could not see at all.
        XINPUT_STATE st{};
        bool answered = false;
        if (g_pad >= 0)
        {
            answered = g_getState(static_cast<DWORD>(g_pad), &st) == ERROR_SUCCESS;
            if (!answered) g_pad = -1;   // let it go and hunt again, it may come back elsewhere
        }
        if (!answered) answered = HuntForPad(now, st);

        if (g_pad != g_loggedPad)
        {
            if (g_pad >= 0)
                GS_LOG("pad: controller on XInput slot %d", g_pad);
            else if (g_loggedPad >= 0)
                GS_LOG("pad: the controller on slot %d is gone", g_loggedPad);
            else
                GS_LOG("pad: nothing on any of the four XInput slots, chord and buzz off. "
                       "A DualSense on USB does not appear here unless Steam Input is on.");
            g_loggedPad = g_pad;
        }

        const bool was = g_connected;
        g_connected = answered;
        if (!g_connected)
        {
            g_sinceMs[slot] = 0;
            g_fired[slot] = false;
            // The pad left mid-buzz. Forget that the motors were running, or
            // the next Pump after it comes back sees the state it wanted and
            // makes no call.
            if (was) g_buzzing = false;
            return false;
        }

        const bool held = (st.Gamepad.wButtons & buttons) == buttons;
        if (!held) { g_sinceMs[slot] = 0; g_fired[slot] = false; return false; }

        if (!g_sinceMs[slot]) g_sinceMs[slot] = now ? now : 1;
        if (g_fired[slot]) return false;
        if (now - g_sinceMs[slot] < holdMs) return false;
        g_fired[slot] = true;
        return true;
    }

    void Buzz(uint16_t strength, uint32_t ms)
    {
        const uint32_t now = GetTickCount();
        g_buzzStrength.store(strength);
        g_buzzUntil.store(now + ms ? now + ms : 1);
    }

    void Pump()
    {
        if (!g_setState || g_pad < 0) return;
        const uint32_t until = g_buzzUntil.load();
        const bool want = until != 0 && static_cast<int32_t>(GetTickCount() - until) < 0;
        if (want == g_buzzing) return;
        g_buzzing = want;
        XINPUT_VIBRATION v{};
        if (want)
        {
            v.wLeftMotorSpeed = g_buzzStrength.load();
            v.wRightMotorSpeed = g_buzzStrength.load();
        }
        g_setState(static_cast<DWORD>(g_pad), &v);
    }

    bool Connected() { return g_connected; }
}
