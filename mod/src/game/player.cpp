#include "game/player.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "game/rtti.h"

namespace
{
    // Master Looter's offsets for build 2.01.00, signatures.h in that project.
    constexpr uintptr_t kOff_Comp_Owner    = 0x08;   // component -> actor, to be confirmed
    constexpr uintptr_t kOff_Ent_Comps     = 0x68;   // actor -> component block
    constexpr uintptr_t kComps_SlotsEnd    = 0x80;   // component pointers live in [0, 0x80)
    constexpr uintptr_t kOff_Comps_Transform = 0x1A0;
    constexpr uintptr_t kOff_Tf_Pos        = 0xB4;
    constexpr uintptr_t kOff_Tf_ParentEid  = 0xC8;
    constexpr uintptr_t kOff_Tf_ParentPos  = 0xEC;

    std::atomic<void*> g_comp{nullptr};
    std::mutex g_mutex;
    gs::player::Pos g_last;
    int g_describeLeft = 3;   // first few reads log the whole walk

    uintptr_t Deref(uintptr_t at)
    {
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), sizeof(uintptr_t))) return 0;
        return *reinterpret_cast<const uintptr_t*>(at);
    }

    const char* NameOf(uintptr_t obj)
    {
        const uintptr_t vt = Deref(obj);
        const char* n = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
        return n ? n : "(no rtti)";
    }

    // The whole walk in one guarded leaf, plain data out.
    bool Walk(uintptr_t comp, float* out, uintptr_t* actorOut, uintptr_t* tfOut, uint32_t* parentOut)
    {
        __try
        {
            const uintptr_t actor = Deref(comp + kOff_Comp_Owner);
            if (!actor) return false;
            const uintptr_t comps = Deref(actor + kOff_Ent_Comps);
            if (!comps) return false;
            const uintptr_t tf = Deref(comps + kOff_Comps_Transform);
            if (!tf) return false;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(tf), kOff_Tf_ParentPos + 12)) return false;

            float v[3], pw[3];
            memcpy(v, reinterpret_cast<const void*>(tf + kOff_Tf_Pos), sizeof(v));
            const uint32_t parent = *reinterpret_cast<const uint32_t*>(tf + kOff_Tf_ParentEid);
            if (parent != 0xFFFFFFFF && parent != 0)
            {
                memcpy(pw, reinterpret_cast<const void*>(tf + kOff_Tf_ParentPos), sizeof(pw));
                if (std::isfinite(pw[0]) && std::isfinite(pw[1]) && std::isfinite(pw[2]))
                {
                    v[0] += pw[0]; v[1] += pw[1]; v[2] += pw[2];
                }
            }
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
            out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
            *actorOut = actor;
            *tfOut = tf;
            *parentOut = parent;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void Describe(uintptr_t comp, uintptr_t actor, uintptr_t tf)
    {
        GS_LOG("[player] component 0x%p is %s", reinterpret_cast<void*>(comp), NameOf(comp));
        GS_LOG("[player]   +0x08 -> 0x%p is %s", reinterpret_cast<void*>(actor), NameOf(actor));
        const uintptr_t comps = Deref(actor + kOff_Ent_Comps);
        GS_LOG("[player]   +0x68 -> 0x%p component block", reinterpret_cast<void*>(comps));
        for (uintptr_t off = 0; comps && off < kComps_SlotsEnd; off += 8)
        {
            const uintptr_t c = Deref(comps + off);
            if (c) GS_LOG("[player]     slot +0x%02llX 0x%p %s",
                          static_cast<unsigned long long>(off), reinterpret_cast<void*>(c), NameOf(c));
        }
        GS_LOG("[player]   block+0x1A0 -> 0x%p transform, %s", reinterpret_cast<void*>(tf), NameOf(tf));
    }
}

namespace gs::player
{
    void SetSpecialComponent(void* comp) { g_comp.store(comp); }

    Pos Read()
    {
        Pos p;
        const auto comp = reinterpret_cast<uintptr_t>(g_comp.load());
        if (!comp) return p;

        float v[3]{};
        uintptr_t actor = 0, tf = 0;
        uint32_t parent = 0;
        if (!Walk(comp, v, &actor, &tf, &parent))
        {
            if (g_describeLeft > 0)
            {
                --g_describeLeft;
                GS_LOG("[player] walk failed from component 0x%p; +0x08 -> 0x%p is %s",
                       reinterpret_cast<void*>(comp),
                       reinterpret_cast<void*>(Deref(comp + kOff_Comp_Owner)),
                       NameOf(Deref(comp + kOff_Comp_Owner)));
            }
            return p;
        }

        // A world position on this map is thousands of units from the origin
        // and never astronomically far. Anything else is a wrong offset.
        const float mag = std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[2]);
        if (mag > 1.0e6f)
        {
            if (g_describeLeft > 0)
            {
                --g_describeLeft;
                GS_LOG("[player] walk gave (%.1f, %.1f, %.1f), not a world position", v[0], v[1], v[2]);
                Describe(comp, actor, tf);
            }
            return p;
        }

        if (g_describeLeft > 0)
        {
            --g_describeLeft;
            Describe(comp, actor, tf);
            GS_LOG_OK("[player] position (%.3f, %.3f, %.3f), parent id 0x%08X",
                      v[0], v[1], v[2], parent);
        }

        p.x = v[0]; p.y = v[1]; p.z = v[2]; p.valid = true;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_last = p;
        return p;
    }

    Pos Last()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_last;
    }
}
