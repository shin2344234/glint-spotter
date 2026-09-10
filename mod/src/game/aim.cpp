#include "game/aim.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/rtti.h"

namespace
{
    constexpr uintptr_t kOff_Ent_Comps       = 0x68;
    constexpr uintptr_t kOff_Comps_Transform = 0x1A0;
    constexpr uintptr_t kOff_Tf_Pos          = 0xB4;
    constexpr uintptr_t kOff_Tf_ParentEid    = 0xC8;
    constexpr uintptr_t kOff_Tf_ParentPos    = 0xEC;
    constexpr uintptr_t kOff_Special_Active  = 0x40;   // player id while the flash is on
    constexpr size_t    kSearchBytes         = 0x800;  // how far into a component to look

    std::atomic<uintptr_t> g_player{0};
    std::atomic<uintptr_t> g_detect{0};
    std::atomic<uintptr_t> g_special{0};
    int g_describeLeft = 4;

    uintptr_t Deref(uintptr_t at)
    {
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), sizeof(uintptr_t))) return 0;
        return *reinterpret_cast<const uintptr_t*>(at);
    }

    const char* NameOf(uintptr_t obj)
    {
        const uintptr_t vt = Deref(obj);
        return vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
    }

    bool IsActorClass(const char* n)
    {
        // Every actor class in this binary ends in "Actor@pa@@" or has "Actor@"
        // in its namespace tail; components end in "ActorComponent@pa@@" and
        // must not count.
        if (!n) return false;
        if (strstr(n, "ActorComponent@")) return false;
        return strstr(n, "Actor@") != nullptr;
    }

    bool PositionOf(uintptr_t actor, float* out)
    {
        __try
        {
            const uintptr_t comps = Deref(actor + kOff_Ent_Comps);
            if (!comps) return false;
            const uintptr_t tf = Deref(comps + kOff_Comps_Transform);
            if (!tf || !gs::rtti::Readable(reinterpret_cast<const void*>(tf), kOff_Tf_ParentPos + 12)) return false;
            float v[3], pw[3];
            memcpy(v, reinterpret_cast<const void*>(tf + kOff_Tf_Pos), sizeof(v));
            const uint32_t parent = *reinterpret_cast<const uint32_t*>(tf + kOff_Tf_ParentEid);
            if (parent != 0xFFFFFFFF && parent != 0)
            {
                memcpy(pw, reinterpret_cast<const void*>(tf + kOff_Tf_ParentPos), sizeof(pw));
                if (std::isfinite(pw[0]) && std::isfinite(pw[1]) && std::isfinite(pw[2]))
                { v[0] += pw[0]; v[1] += pw[1]; v[2] += pw[2]; }
            }
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
            if (std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[2]) > 1.0e6f) return false;
            out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Walk one component's pointer fields. Fills out on the first actor that is
    // not the player. Plain data in and out so the frame can hold the handler.
    bool Search(uintptr_t comp, uintptr_t player, bool describe, gs::aim::Target* out)
    {
        __try
        {
            if (!comp || !gs::rtti::Readable(reinterpret_cast<const void*>(comp), 0x40)) return false;
            size_t bytes = kSearchBytes;
            while (bytes > 0x40 && !gs::rtti::Readable(reinterpret_cast<const void*>(comp), bytes)) bytes /= 2;

            for (uintptr_t off = 0x10; off + 8 <= bytes; off += 8)
            {
                const uintptr_t p = *reinterpret_cast<const uintptr_t*>(comp + off);
                if (p < 0x10000 || (p & 7) != 0 || p == player) continue;
                const char* n = NameOf(p);
                if (!n) continue;
                if (describe) GS_LOG("[aim]   +0x%03llX -> 0x%p %s", static_cast<unsigned long long>(off),
                                     reinterpret_cast<void*>(p), n);
                if (!IsActorClass(n)) continue;

                float v[3];
                if (!PositionOf(p, v)) continue;
                out->x = v[0]; out->y = v[1]; out->z = v[2];
                out->actor = p;
                out->foundAt = off;
                strncpy_s(out->cls, sizeof(out->cls), n, _TRUNCATE);
                out->valid = true;
                return true;
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

namespace gs::aim
{
    void SetPlayerActor(uintptr_t actor) { g_player.store(actor); }
    void SetDetectComponent(uintptr_t comp) { g_detect.store(comp); }
    void SetSpecialComponent(uintptr_t comp) { g_special.store(comp); }

    bool FlashActive()
    {
        const uintptr_t sp = g_special.load();
        if (!sp) return false;
        const uintptr_t at = sp + kOff_Special_Active;
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 4)) return false;
        return *reinterpret_cast<const uint32_t*>(at) != 0;
    }

    Target Resolve()
    {
        Target t;
        const uintptr_t player = g_player.load();
        const uintptr_t detect = g_detect.load();
        const uintptr_t special = g_special.load();
        const bool describe = g_describeLeft > 0;
        if (describe) --g_describeLeft;

        if (describe) GS_LOG("[aim] detect component 0x%p, special 0x%p, player 0x%p, flash %s",
                             reinterpret_cast<void*>(detect), reinterpret_cast<void*>(special),
                             reinterpret_cast<void*>(player), FlashActive() ? "on" : "off");

        if (detect && Search(detect, player, describe, &t))
        {
            GS_LOG_OK("[aim] target via detect component +0x%llX: %s at (%.3f, %.3f, %.3f)",
                      static_cast<unsigned long long>(t.foundAt), t.cls, t.x, t.y, t.z);
            return t;
        }
        if (special && Search(special, player, describe, &t))
        {
            GS_LOG_OK("[aim] target via special component +0x%llX: %s at (%.3f, %.3f, %.3f)",
                      static_cast<unsigned long long>(t.foundAt), t.cls, t.x, t.y, t.z);
            return t;
        }
        GS_LOG("[aim] no actor pointer in either component right now");
        return t;
    }
}
