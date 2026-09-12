#include "hook/iathook.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

#include "core/log.h"

namespace
{
    // The import table describes two parallel arrays per DLL. The first holds
    // the names the linker wrote down and never changes; the second is what
    // the loader overwrote with real addresses, and is the one worth writing
    // to. An import brought in by ordinal has the top bit set in the name
    // array and no name to compare, so it is skipped.
    bool Match(const char* a, const char* b, bool fold)
    {
        if (!a || !b) return false;
        return fold ? _stricmp(a, b) == 0 : strcmp(a, b) == 0;
    }
}

namespace gs::iathook
{
    bool Swap(const char* dll, const char* fn, void* detour, void** previous)
    {
        if (previous) *previous = nullptr;
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        if (!base || !detour || !previous) return false;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

        const IMAGE_DATA_DIRECTORY& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || !dir.Size) return false;

        for (const auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                 base + dir.VirtualAddress);
             imp->Name; ++imp)
        {
            if (!Match(reinterpret_cast<const char*>(base + imp->Name), dll, true)) continue;

            const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(
                base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);

            for (; names->u1.AddressOfData; ++names, ++slots)
            {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto* by = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    base + names->u1.AddressOfData);
                if (!Match(by->Name, fn, false)) continue;

                DWORD old = 0;
                if (!VirtualProtect(slots, sizeof(*slots), PAGE_READWRITE, &old))
                {
                    GS_LOG_ERR("[iat] %s is imported but its table entry will not take a write",
                               fn);
                    return false;
                }
                *previous = reinterpret_cast<void*>(slots->u1.Function);
                slots->u1.Function = reinterpret_cast<ULONGLONG>(detour);
                VirtualProtect(slots, sizeof(*slots), old, &old);
                return true;
            }
        }
        return false;
    }
}
