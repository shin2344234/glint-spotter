#include "game/typescan.h"

#include <Windows.h>
#include <cstring>

#include "game/rtti.h"

namespace
{
    // x64 RTTICompleteObjectLocator, same layout rtti.cpp documents.
    struct CompleteObjectLocator
    {
        uint32_t signature;
        uint32_t offset;
        uint32_t cdOffset;
        uint32_t pTypeDescriptor;
        uint32_t pClassDescriptor;
        uint32_t pSelf;
    };

    constexpr size_t kTypeDescriptorNameOffset = 0x10;
    constexpr size_t kMaxLocators = 4096;

    bool Contains(const char* haystack, const char* needle)
    {
        return haystack && needle && strstr(haystack, needle) != nullptr;
    }

    // Count the leading entries of a vtable that look like code in this module.
    // Only a sanity signal for the log; a wrong count changes nothing.
    int CountSlots(uintptr_t vtable, uintptr_t base, size_t size)
    {
        int n = 0;
        for (; n < 512; ++n)
        {
            const uintptr_t slot = vtable + static_cast<uintptr_t>(n) * sizeof(void*);
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(slot), sizeof(void*))) break;
            const uintptr_t fn = *reinterpret_cast<const uintptr_t*>(slot);
            if (fn < base || fn >= base + size) break;
        }
        return n;
    }
}

namespace gs::typescan
{
    bool ModuleRange(uintptr_t& base, size_t& size)
    {
        const HMODULE h = GetModuleHandleW(nullptr);
        if (!h) return false;
        base = reinterpret_cast<uintptr_t>(h);

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!rtti::Readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (!rtti::Readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return false;

        size = nt->OptionalHeader.SizeOfImage;
        return size != 0;
    }

    size_t FindClasses(const char* const* keywords, size_t keywordCount,
                       ClassInfo* out, size_t capacity)
    {
        if (!keywords || keywordCount == 0 || !out || capacity == 0) return 0;

        uintptr_t base = 0;
        size_t size = 0;
        if (!ModuleRange(base, size)) return 0;

        // Pass one: every locator in the image whose name we care about. A
        // locator is self-identifying, signature 1 and a self-RVA pointing back
        // at itself, which is specific enough that arbitrary data does not match.
        size_t found = 0;
        __try
        {
            for (uint32_t rva = 0; rva + sizeof(CompleteObjectLocator) < size && found < capacity;
                 rva += 4)
            {
                const auto* col = reinterpret_cast<const CompleteObjectLocator*>(base + rva);
                if (col->signature != 1) continue;
                if (col->pSelf != rva) continue;
                if (col->pTypeDescriptor == 0 || col->pTypeDescriptor >= size) continue;

                const char* name =
                    reinterpret_cast<const char*>(base + col->pTypeDescriptor + kTypeDescriptorNameOffset);
                if (name[0] != '.' || name[1] != '?') continue;

                bool wanted = false;
                for (size_t k = 0; k < keywordCount && !wanted; ++k)
                    wanted = Contains(name, keywords[k]);
                if (!wanted) continue;

                ClassInfo& ci = out[found];
                strncpy_s(ci.name, sizeof(ci.name), name, _TRUNCATE);
                ci.colVa = base + rva;
                ++found;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // A section that is not really readable ends the walk. Whatever was
            // collected up to here still stands.
        }

        if (found == 0) return 0;

        // Pass two: the vtable for each. MSVC puts the locator pointer one slot
        // before the vtable, so a qword equal to a locator address has the vtable
        // immediately after it.
        __try
        {
            const uintptr_t* p = reinterpret_cast<const uintptr_t*>(base);
            const size_t count = (size - sizeof(void*)) / sizeof(uintptr_t);
            for (size_t i = 0; i < count; ++i)
            {
                const uintptr_t v = p[i];
                if (v < base || v >= base + size) continue;
                for (size_t c = 0; c < found; ++c)
                {
                    if (out[c].vtableVa || v != out[c].colVa) continue;
                    out[c].vtableVa = reinterpret_cast<uintptr_t>(&p[i + 1]);
                    out[c].vtableRva = static_cast<uint32_t>(out[c].vtableVa - base);
                    out[c].slots = CountSlots(out[c].vtableVa, base, size);
                    break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        return found;
    }
}
