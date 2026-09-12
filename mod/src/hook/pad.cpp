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
    std::atomic<uint32_t> g_buzzUntil{0};
    std::atomic<uint16_t> g_buzzStrength{0};
    bool g_buzzing = false;
    using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
    SetStateFn g_setState = nullptr;
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
        XINPUT_STATE st{};
        const DWORD rc = g_getState(0, &st);
        const bool was = g_connected;
        g_connected = rc == ERROR_SUCCESS;
        if (g_connected != was) GS_LOG("pad: controller %s", g_connected ? "connected" : "gone");
        if (!g_connected) { g_sinceMs[slot] = 0; g_fired[slot] = false; return false; }

        const bool held = (st.Gamepad.wButtons & buttons) == buttons;
        if (!held) { g_sinceMs[slot] = 0; g_fired[slot] = false; return false; }

        const uint32_t now = GetTickCount();
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
        if (!g_setState || !g_connected) return;
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
        g_setState(0, &v);
    }

    bool Connected() { return g_connected; }
}
