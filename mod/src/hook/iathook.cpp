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

    int SwapAll(const char* dll, unsigned ordinal, const char* fn, void* detour, void** previous,
                void** where[], int max, int* differs)
    {
        if (previous) *previous = nullptr;
        if (differs) *differs = 0;
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        if (!base || !detour || !previous) return 0;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

        const IMAGE_DATA_DIRECTORY& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || !dir.Size) return 0;

        // Every matching entry first, so `previous` is set from the first one
        // before any of them is written.
        IMAGE_THUNK_DATA* found[8]{};
        int nFound = 0;
        for (const auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                 base + dir.VirtualAddress);
             imp->Name && nFound < 8; ++imp)
        {
            if (!Match(reinterpret_cast<const char*>(base + imp->Name), dll, true)) continue;
            // With no name array, the only array left already holds bound
            // addresses, and reading one as a name offset reads anywhere.
            // Both XInput descriptors in 3576 have one.
            if (!imp->OriginalFirstThunk) continue;

            const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);

            for (; names->u1.AddressOfData && nFound < 8; ++names, ++slots)
            {
                bool hit = false;
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                    hit = ordinal && IMAGE_ORDINAL(names->u1.Ordinal) == ordinal;
                else if (fn)
                    hit = Match(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                                    base + names->u1.AddressOfData)->Name, fn, false);
                if (hit) found[nFound++] = slots;
            }
        }
        if (!nFound) return 0;

        void* first = reinterpret_cast<void*>(found[0]->u1.Function);
        *previous = first;
        int took = 0;
        for (int i = 0; i < nFound; ++i)
        {
            IMAGE_THUNK_DATA* s = found[i];
            if (reinterpret_cast<void*>(s->u1.Function) != first)
            {
                if (differs) ++*differs;
                continue;
            }
            DWORD old = 0;
            if (!VirtualProtect(s, sizeof(*s), PAGE_READWRITE, &old))
            {
                GS_LOG_ERR("[iat] an entry for %s ordinal %u will not take a write", dll, ordinal);
                continue;
            }
            s->u1.Function = reinterpret_cast<ULONGLONG>(detour);
            VirtualProtect(s, sizeof(*s), old, &old);
            if (where && took < max) where[took] = reinterpret_cast<void**>(&s->u1.Function);
            ++took;
        }
        return took;
    }

    void Restore(void** const where[], int n, void* detour, void* original)
    {
        for (int i = 0; i < n; ++i)
        {
            void** at = where[i];
            if (!at || *at != detour) continue;
            DWORD old = 0;
            if (!VirtualProtect(at, sizeof(*at), PAGE_READWRITE, &old)) continue;
            *at = original;
            VirtualProtect(at, sizeof(*at), old, &old);
        }
    }
}
