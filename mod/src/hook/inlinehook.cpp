#include "hook/inlinehook.h"

#include <Windows.h>
#include <cstring>

#include "core/log.h"

namespace
{
    constexpr size_t kJumpBytes = 14;

    // FF 25 00 00 00 00 <qword>: jump to the address stored right after the
    // instruction. No displacement to fit, so the detour can live anywhere.
    void WriteAbsoluteJump(uint8_t* at, uintptr_t to)
    {
        at[0] = 0xFF;
        at[1] = 0x25;
        at[2] = 0x00; at[3] = 0x00; at[4] = 0x00; at[5] = 0x00;
        memcpy(at + 6, &to, 8);
    }
}

namespace gs::inlinehook
{
    bool Install(Hook& h, uintptr_t target, size_t len, void* detour,
                 const uint8_t* expect, size_t expectLen)
    {
        if (h.installed) return true;
        if (len < kJumpBytes || len > sizeof(h.saved) || !target || !detour) return false;

        auto* code = reinterpret_cast<uint8_t*>(target);
        if (memcmp(code, expect, expectLen) != 0)
        {
            GS_LOG_ERR("[hook] +0x%llX does not start with the bytes this build was written "
                       "against; nothing patched",
                       static_cast<unsigned long long>(target - reinterpret_cast<uintptr_t>(
                           GetModuleHandleW(nullptr))));
            return false;
        }

        auto* tramp = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, len + kJumpBytes, MEM_COMMIT | MEM_RESERVE,
                         PAGE_EXECUTE_READWRITE));
        if (!tramp) return false;
        memcpy(tramp, code, len);
        WriteAbsoluteJump(tramp + len, target + len);

        DWORD old = 0;
        if (!VirtualProtect(code, len, PAGE_EXECUTE_READWRITE, &old))
        {
            VirtualFree(tramp, 0, MEM_RELEASE);
            return false;
        }
        memcpy(h.saved, code, len);
        WriteAbsoluteJump(code, reinterpret_cast<uintptr_t>(detour));
        // Anything between the jump and the next instruction boundary would be
        // the tail of an instruction that no longer starts where it did, so it
        // is filled with single byte nops. Nothing jumps into the middle of a
        // prologue, but a debugger reading it should see something sane.
        for (size_t i = kJumpBytes; i < len; ++i) code[i] = 0x90;
        VirtualProtect(code, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), code, len);

        h.target = target;
        h.trampoline = tramp;
        h.len = len;
        h.installed = true;
        return true;
    }

    void Remove(Hook& h)
    {
        if (!h.installed) return;
        auto* code = reinterpret_cast<uint8_t*>(h.target);
        DWORD old = 0;
        if (VirtualProtect(code, h.len, PAGE_EXECUTE_READWRITE, &old))
        {
            memcpy(code, h.saved, h.len);
            VirtualProtect(code, h.len, old, &old);
            FlushInstructionCache(GetCurrentProcess(), code, h.len);
        }
        if (h.trampoline) VirtualFree(h.trampoline, 0, MEM_RELEASE);
        h.trampoline = nullptr;
        h.installed = false;
    }
}
