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
    int g_describeLeft = 8;
    uint64_t g_press = 0;

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

    // The target is not a pointer to an actor: session sixteen's task held the
    // player, a Havok simulation, and nothing else with RTTI. In this engine
    // things are keyed by actor id, A0100001 style, so the target is a dword.
    // Dump the whole object so two presses, one aimed and one not, give the
    // field by diff, and flag every dword shaped like an id on the way.
    bool LooksLikeActorId(uint32_t v)
    {
        const uint32_t top = v >> 24;
        return (top == 0xA0 || top == 0xB0 || top == 0xA1 || top == 0xB1) && (v & 0x00FFFFFF) != 0;
    }

    void DumpObject(const char* tag, uintptr_t obj, size_t bytes)
    {
        size_t n = bytes;
        while (n >= 0x40 && !gs::rtti::Readable(reinterpret_cast<const void*>(obj), n)) n /= 2;
        if (n < 0x40) return;
        const auto* q = reinterpret_cast<const uint32_t*>(obj);
        for (size_t off = 0; off + 32 <= n; off += 32)
        {
            const size_t i = off / 4;
            GS_LOG("[dump %s +%03zX] %08X %08X %08X %08X %08X %08X %08X %08X", tag, off,
                   q[i], q[i+1], q[i+2], q[i+3], q[i+4], q[i+5], q[i+6], q[i+7]);
        }
        for (size_t off = 0; off + 4 <= n; off += 4)
        {
            const uint32_t v = q[off / 4];
            if (LooksLikeActorId(v)) GS_LOG("[dump %s] actor id shaped dword at +0x%03zX: %08X", tag, off, v);
        }
    }

    void DumpHeader(const char* tag, uintptr_t obj, size_t bytes) { DumpObject(tag, obj, bytes); }

    // Walk one object's pointer fields. Fills out on the first actor that is
    // not the player. Depth 1 also searches pointees that look like holders:
    // the detect component keeps a FindDetectTargetTask at +0x1D0, and the
    // target is in the task, not on the component. Plain data in and out so
    // the frame can hold the handler.
    bool Search(uintptr_t obj, uintptr_t player, bool describe, int depth, const char* tag,
                gs::aim::Target* out)
    {
        __try
        {
            if (!obj || !gs::rtti::Readable(reinterpret_cast<const void*>(obj), 0x40)) return false;
            size_t bytes = kSearchBytes;
            while (bytes > 0x40 && !gs::rtti::Readable(reinterpret_cast<const void*>(obj), bytes)) bytes /= 2;

            for (uintptr_t off = 0x08; off + 8 <= bytes; off += 8)
            {
                const uintptr_t p = *reinterpret_cast<const uintptr_t*>(obj + off);
                if (p < 0x10000 || (p & 7) != 0 || p == player || p == obj) continue;
                const char* n = NameOf(p);
                if (!n) continue;
                if (describe) GS_LOG("[aim]   %s+0x%03llX -> 0x%p %s", tag,
                                     static_cast<unsigned long long>(off), reinterpret_cast<void*>(p), n);
                if (IsActorClass(n))
                {
                    float v[3];
                    if (!PositionOf(p, v)) continue;
                    out->x = v[0]; out->y = v[1]; out->z = v[2];
                    out->actor = p;
                    out->foundAt = off;
                    strncpy_s(out->cls, sizeof(out->cls), n, _TRUNCATE);
                    out->valid = true;
                    return true;
                }
                // One level down into anything that could hold a result.
                if (depth > 0 && (strstr(n, "Task") || strstr(n, "Target") || strstr(n, "Detect") ||
                                  strstr(n, "IRefCounted")))
                {
                    if (describe && strstr(n, "FindDetectTargetTask")) DumpHeader("task", p, 0x300);
                    if (Search(p, player, describe, depth - 1, "task", out)) return true;
                }
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

        ++g_press;
        if (describe) GS_LOG("[aim] press %llu: detect component 0x%p, special 0x%p, player 0x%p, flash %s",
                             static_cast<unsigned long long>(g_press),
                             reinterpret_cast<void*>(detect), reinterpret_cast<void*>(special),
                             reinterpret_cast<void*>(player), FlashActive() ? "on" : "off");
        if (describe && detect) DumpObject("detect", detect, 0x800);
        if (describe && special) DumpObject("special", special, 0x400);

        if (detect && Search(detect, player, describe, 1, "detect", &t))
        {
            GS_LOG_OK("[aim] target via detect component +0x%llX: %s at (%.3f, %.3f, %.3f)",
                      static_cast<unsigned long long>(t.foundAt), t.cls, t.x, t.y, t.z);
            return t;
        }
        if (special && Search(special, player, describe, 1, "special", &t))
        {
            GS_LOG_OK("[aim] target via special component +0x%llX: %s at (%.3f, %.3f, %.3f)",
                      static_cast<unsigned long long>(t.foundAt), t.cls, t.x, t.y, t.z);
            return t;
        }
        GS_LOG("[aim] no actor pointer in either component right now");
        return t;
    }

    bool AimPointLocal(uintptr_t charctl, float lx, float ly, float lz, float* out)
    {
        constexpr uintptr_t kOff_CharCtl_Aim = 0x318;
        if (!charctl) return false;
        const uintptr_t at = charctl + kOff_CharCtl_Aim;
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 12)) return false;
        float f[3];
        __try
        {
            memcpy(f, reinterpret_cast<const void*>(at), 12);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) return false;
        const float dx = f[0] - lx, dy = f[1] - ly, dz = f[2] - lz;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < 0.25f || d > 150.0f) return false;
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
        return true;
    }

    uintptr_t DetectTask()
    {
        const uintptr_t detect = g_detect.load();
        if (!detect) return 0;
        // Session sixteen: the task sits at +0x1D0. Verified by name before use.
        const uintptr_t p = Deref(detect + 0x1D0);
        const char* n = p ? NameOf(p) : nullptr;
        return (n && strstr(n, "FindDetectTargetTask")) ? p : 0;
    }
}
