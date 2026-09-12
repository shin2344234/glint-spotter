#include "game/pinmodel.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/player.h"
#include "game/rtti.h"
#include "game/signatures.h"

namespace
{
    // Forty lists, sixteen bytes of header each, which is what the create
    // call's own arithmetic reaches. They were printed with names for a
    // while, from an alert kind enum that has nothing to do with this
    // submodule, and the names are gone because they were fiction.
    constexpr int kKindCount = 40;
}

namespace gs::pinmodel
{
    uintptr_t Submodule()
    {
        const uintptr_t actor = gs::player::Actor();
        if (!actor) return 0;
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(actor + gs::sig::kOff_Actor_Components), 8))
                return 0;
            const uintptr_t comps =
                *reinterpret_cast<const uintptr_t*>(actor + gs::sig::kOff_Actor_Components);
            if (comps < 0x10000) return 0;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(comps + gs::sig::kOff_Comp_PinSubmodule), 8))
                return 0;
            const uintptr_t sub =
                *reinterpret_cast<const uintptr_t*>(comps + gs::sig::kOff_Comp_PinSubmodule);
            if (sub < 0x10000 || (sub & 7) != 0) return 0;
            // The headers run to 0xC8 + forty kinds of sixteen bytes.
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(sub), 0xC8 + 40 * 16)) return 0;
            return sub;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    List Read(int kind) { return ReadAt(Submodule(), kind); }

    List ReadAt(uintptr_t sub, int kind)
    {
        List out;
        if (!sub || kind < 0 || kind >= 64) return out;
        __try
        {
            out.header = sub + gs::sig::kOff_Pin_Lists + static_cast<uintptr_t>(kind) * 16;
            out.data = *reinterpret_cast<const uintptr_t*>(out.header);
            out.count = *reinterpret_cast<const uint32_t*>(out.header + 8);
            // An empty list is a valid answer and says nothing is wrong; a
            // count with no storage behind it, or storage that cannot be read
            // for the whole run of records, is a wrong offset.
            if (out.count == 0) { out.ok = out.data == 0 || (out.data & 7) == 0; return out; }
            if (out.data < 0x10000 || (out.data & 7) != 0) return out;
            if (out.count > 4096) return out;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(out.data),
                                    static_cast<size_t>(out.count) * gs::sig::kPinRecord))
                return out;
            out.ok = true;
            return out;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out.ok = false;
            return out;
        }
    }

    void LogState(const char* why)
    {
        const uintptr_t sub = Submodule();
        if (!sub)
        {
            GS_LOG("[pins] %s: no submodule at *(*(actor + 0x%llX) + 0x%llX) yet", why,
                   static_cast<unsigned long long>(gs::sig::kOff_Actor_Components),
                   static_cast<unsigned long long>(gs::sig::kOff_Comp_PinSubmodule));
            return;
        }
        const char* cls = nullptr;
        __try
        {
            cls = gs::rtti::VtableClassName(*reinterpret_cast<const void* const*>(sub));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        GS_LOG("[pins] %s: submodule 0x%p%s%s", why, reinterpret_cast<void*>(sub),
               cls ? ", class " : "", cls ? cls : "");

        // Numbered, not named. Only lists 0 and 1 ever hold markers, and
        // printing "FactionBlockade" beside list 21 sent four builds
        // looking in the wrong place.
        int nonEmpty = 0;
        for (int k = 0; k < kKindCount; ++k)
        {
            const List l = Read(k);
            if (!l.ok || l.count == 0) continue;
            ++nonEmpty;
            GS_LOG("[pins]   list %2d: %u record(s) at 0x%p", k, l.count,
                   reinterpret_cast<void*>(l.data));
        }
        if (!nonEmpty) GS_LOG("[pins]   every list is empty");
    }
}
