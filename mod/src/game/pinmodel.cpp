#include "game/pinmodel.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>

#include "core/log.h"
#include "game/player.h"
#include "game/rtti.h"
#include "game/signatures.h"

namespace
{
    // A record is {int64 id, float x, y, z, byte, byte} in 24 bytes. The map
    // runs to about thirteen thousand units from the origin in x and z and the
    // ground sits between roughly zero and fifteen hundred, so a record that
    // is really a record reads as a position and one that is not reads as
    // nonsense. This is the same check the level gimmick table's quaternion
    // gave for free: the data validates the offset.
    bool LooksLikePosition(float x, float y, float z)
    {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
        if (std::fabs(x) > 40000.0f || std::fabs(z) > 40000.0f) return false;
        if (y < -2000.0f || y > 8000.0f) return false;
        return std::fabs(x) + std::fabs(z) > 1.0f;
    }
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
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(sub), 0x400)) return 0;
            return sub;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void LogState(const char* why)
    {
        const uintptr_t sub = Submodule();
        if (!sub)
        {
            GS_LOG("[pins] %s: no submodule at actor+0x%llX -> +0x%llX yet", why,
                   static_cast<unsigned long long>(gs::sig::kOff_Actor_Components),
                   static_cast<unsigned long long>(gs::sig::kOff_Comp_PinSubmodule));
            return;
        }
        const char* cls = gs::rtti::VtableClassName(
            *reinterpret_cast<const void* const*>(sub));
        GS_LOG("[pins] %s: submodule 0x%p%s%s", why, reinterpret_cast<void*>(sub),
               cls ? ", class " : "", cls ? cls : "");

        __try
        {
            // The list the notes name, and its neighbours. If 0xC8 plus kind
            // times sixteen is right then exactly one of these reads as a
            // vector of positions; if it is wrong, none of them will, and the
            // sweep says where to look instead.
            const uintptr_t want = gs::sig::kOff_Pin_Lists +
                                   static_cast<uintptr_t>(gs::sig::kPinKind) * 16;
            for (uintptr_t off = 0x40; off + 16 <= 0x400; off += 8)
            {
                const uintptr_t data = *reinterpret_cast<const uintptr_t*>(sub + off);
                const uint32_t count = *reinterpret_cast<const uint32_t*>(sub + off + 8);
                const uint32_t cap   = *reinterpret_cast<const uint32_t*>(sub + off + 12);
                if (data < 0x10000 || (data & 7) != 0) continue;
                if (count == 0 || count > 4096 || cap < count || cap > 65536) continue;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(data),
                                        static_cast<size_t>(count) * gs::sig::kPinRecord))
                    continue;

                int good = 0;
                for (uint32_t i = 0; i < count && i < 64; ++i)
                {
                    const auto* r = reinterpret_cast<const uint8_t*>(data) +
                                    static_cast<size_t>(i) * gs::sig::kPinRecord;
                    float x, y, z;
                    memcpy(&x, r + 8, 4); memcpy(&y, r + 12, 4); memcpy(&z, r + 16, 4);
                    if (LooksLikePosition(x, y, z)) ++good;
                }
                if (!good) continue;

                GS_LOG("[pins]   +%03llX: %u record(s) of %u at 0x%p, %d read as positions%s",
                       static_cast<unsigned long long>(off), count, cap,
                       reinterpret_cast<void*>(data), good,
                       off == want ? "   <- where the notes say kind 0x15 lives" : "");
                for (uint32_t i = 0; i < count && i < 12; ++i)
                {
                    const auto* r = reinterpret_cast<const uint8_t*>(data) +
                                    static_cast<size_t>(i) * gs::sig::kPinRecord;
                    int64_t id; float x, y, z;
                    memcpy(&id, r, 8);
                    memcpy(&x, r + 8, 4); memcpy(&y, r + 12, 4); memcpy(&z, r + 16, 4);
                    GS_LOG("[pins]     [%u] id %lld at (%.1f, %.1f, %.1f) flags %02X %02X",
                           i, static_cast<long long>(id), x, y, z, r[20], r[21]);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GS_LOG_ERR("[pins] a read faulted while walking the submodule");
        }
    }
}
