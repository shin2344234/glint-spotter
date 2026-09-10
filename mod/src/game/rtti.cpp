#include "game/rtti.h"

#include <Windows.h>
#include <cstring>

namespace
{
    // x64 RTTICompleteObjectLocator. Signature is 1 on x64 and every pointer
    // field is image-relative, which is what lets us recover the module base
    // from pSelf without asking the loader for it.
    struct CompleteObjectLocator
    {
        uint32_t signature;
        uint32_t offset;
        uint32_t cdOffset;
        uint32_t pTypeDescriptor;
        uint32_t pClassDescriptor;
        uint32_t pSelf;
    };

    // TypeDescriptor is { void* pVFTable; void* spare; char name[]; } so the
    // decorated name starts 0x10 in.
    constexpr size_t kTypeDescriptorNameOffset = 0x10;
}

namespace gs::rtti
{
    bool Readable(const void* p, size_t bytes)
    {
        if (!p || bytes == 0) return false;

        // VirtualQuery reports one region at a time, so a read that straddles a
        // boundary has to check both. Walk until the whole span is covered.
        const uint8_t* cur = static_cast<const uint8_t*>(p);
        const uint8_t* end = cur + bytes;
        while (cur < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(cur, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
            if (mbi.State != MEM_COMMIT) return false;

            const DWORD prot = mbi.Protect & 0xFF;
            const bool readable =
                prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
                prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
            if (!readable) return false;
            // A guard page reads fine once and then raises. Refuse it outright.
            if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;

            const uint8_t* regionEnd =
                static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd <= cur) return false;  // no forward progress, bail
            cur = regionEnd;
        }
        return true;
    }

    // The body below has no C++ objects, so it can sit inside __try. Readable
    // is a pre-check and nothing more: the game frees memory on other threads,
    // and session five died between a Readable that said yes and the read that
    // followed it. Only a handler around the read itself closes that gap.
    const char* VtableClassNameUnguarded(const void* vtable)
    {
        if (!Readable(vtable, sizeof(void*))) return nullptr;

        // The locator pointer lives one slot before the vtable.
        const void* const* slots = static_cast<const void* const*>(vtable);
        if (!Readable(slots - 1, sizeof(void*))) return nullptr;

        const auto colAddr = reinterpret_cast<uintptr_t>(slots[-1]);
        if (!Readable(reinterpret_cast<const void*>(colAddr), sizeof(CompleteObjectLocator)))
            return nullptr;

        const auto* col = reinterpret_cast<const CompleteObjectLocator*>(colAddr);
        if (col->signature != 1) return nullptr;

        // pSelf points back at the locator, which both validates the struct and
        // hands us the module base for the other two RVAs.
        if (col->pSelf == 0 || col->pSelf > colAddr) return nullptr;
        const uintptr_t base = colAddr - col->pSelf;

        const uintptr_t td = base + col->pTypeDescriptor;
        const uintptr_t name = td + kTypeDescriptorNameOffset;
        // A decorated name is short; probing a page is enough and avoids walking
        // off the end of a region looking for a terminator.
        if (!Readable(reinterpret_cast<const void*>(name), 4)) return nullptr;

        const char* s = reinterpret_cast<const char*>(name);
        if (s[0] != '.' || s[1] != '?') return nullptr;
        return s;
    }

    const char* VtableClassName(const void* vtable)
    {
        __try
        {
            return VtableClassNameUnguarded(vtable);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool VtableIs(const void* vtable, const char* decorated)
    {
        __try
        {
            const char* name = VtableClassNameUnguarded(vtable);
            if (!name || !decorated) return false;
            // Bound the comparison so a missing terminator cannot run away.
            return strncmp(name, decorated, 512) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}
