#include "hook/vtable.h"

#include <Windows.h>

#include "game/rtti.h"

namespace gs::vtable
{
    bool Install(uintptr_t vtable, int index, void* replacement, Swap& out, void* volatile* publish)
    {
        out = Swap{};
        if (!vtable || index < 0 || !replacement) return false;

        void** slot = reinterpret_cast<void**>(vtable) + index;
        if (!rtti::Readable(slot, sizeof(void*))) return false;
        if (*slot == replacement) return false;

        // The vtable lives in the image's data section, which is read-only at
        // runtime. Open the one page for the one write, then close it again.
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;

        out.slot = slot;
        out.original = *slot;
        out.replacement = replacement;
        if (publish) *publish = out.original;
        *slot = replacement;
        out.installed = true;

        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), old, &ignored);
        return true;
    }

    bool Restore(Swap& s, bool& leftAlone)
    {
        leftAlone = false;
        if (!s.installed || !s.slot) return false;

        if (*s.slot != s.replacement)
        {
            // Someone stacked on top of us. Restoring would unhook them too.
            leftAlone = true;
            s.installed = false;
            return false;
        }

        DWORD old = 0;
        if (!VirtualProtect(s.slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
        *s.slot = s.original;
        DWORD ignored = 0;
        VirtualProtect(s.slot, sizeof(void*), old, &ignored);
        s.installed = false;
        return true;
    }
}
