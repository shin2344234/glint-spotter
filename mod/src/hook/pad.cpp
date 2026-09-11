#include "hook/pad.h"

#include <Windows.h>
#include <Xinput.h>

#include "core/log.h"

namespace
{
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    GetStateFn g_getState = nullptr;
    uint32_t g_sinceMs = 0;    // when the chord went down, or zero
    bool g_fired = false;      // this hold has already had its turn
    bool g_connected = false;
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
            if (g_getState)
            {
                GS_LOG("pad: using %ls", n);
                return true;
            }
        }
        GS_LOG("pad: no XInput library, controller chord off");
        return false;
    }

    bool ChordHeld(uint16_t buttons, uint32_t holdMs)
    {
        if (!g_getState || !buttons) return false;
        XINPUT_STATE st{};
        const DWORD rc = g_getState(0, &st);
        const bool was = g_connected;
        g_connected = rc == ERROR_SUCCESS;
        if (g_connected != was) GS_LOG("pad: controller %s", g_connected ? "connected" : "gone");
        if (!g_connected) { g_sinceMs = 0; g_fired = false; return false; }

        const bool held = (st.Gamepad.wButtons & buttons) == buttons;
        if (!held) { g_sinceMs = 0; g_fired = false; return false; }

        const uint32_t now = GetTickCount();
        if (!g_sinceMs) g_sinceMs = now ? now : 1;
        if (g_fired) return false;
        if (now - g_sinceMs < holdMs) return false;
        g_fired = true;
        return true;
    }

    bool Connected() { return g_connected; }
}
