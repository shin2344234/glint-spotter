#include "game/player.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "game/rtti.h"
#include "game/actors.h"
#include "game/aim.h"

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
    // Session seventeen: the transform also carries the world position, the
    // one the map draws, at +0x29C, with copies at +0x324, +0x3D0 and +0x51C.
    // +0xB4 is local to the sub-level; the two differed by (-9000, 0, -4000)
    // where the player stood, and a pin placed from the local one landed 9 km
    // from the player on the map.
    constexpr uintptr_t kOff_Tf_WorldPos   = 0x29C;
    // Session eighteen: a yaw quaternion (x, y, z, w) sits right before each
    // position, +0xA4 before the local one and +0x28C before the world one.
    constexpr uintptr_t kOff_Tf_WorldQuat  = 0x28C;

    std::atomic<void*> g_comp{nullptr};
    std::atomic<uintptr_t> g_actor{0};
    std::atomic<uintptr_t> g_detect{0};
    std::atomic<uintptr_t> g_charctl{0};
    std::atomic<bool> g_ownerIsBody{false};
    std::mutex g_mutex;
    gs::player::Pos g_last;
    int g_describeLeft = 3;   // first few reads log the whole walk
    uint32_t g_lostAtMs = 0;  // when the player last stopped answering
    int g_recoverTriesLeft = 8;  // a fault costs one; run out and it stops

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
    // `comp` is the special mode component, which knows the actor at +8. Pass
    // `fromActor` when the actor is already in hand, which is how the mod
    // comes back from a load: the component is not on the player and only the
    // heap sweep ever finds it, but the player himself is in the pools.
    bool Walk(uintptr_t comp, float* out, float* world, uintptr_t* actorOut, uintptr_t* tfOut,
              uint32_t* parentOut, bool fromActor = false)
    {
        __try
        {
            const uintptr_t actor = fromActor ? comp : Deref(comp + kOff_Comp_Owner);
            if (!actor) return false;
            const uintptr_t comps = Deref(actor + kOff_Ent_Comps);
            if (!comps) return false;
            const uintptr_t tf = Deref(comps + kOff_Comps_Transform);
            if (!tf) return false;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(tf), kOff_Tf_WorldPos + 12)) return false;
            memcpy(world, reinterpret_cast<const void*>(tf + kOff_Tf_WorldPos), 12);
            memcpy(world + 3, reinterpret_cast<const void*>(tf + kOff_Tf_WorldQuat), 16);

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

    bool Recover()
    {
        // While the one we have still answers there is nothing to do, and this
        // is also the cheap path that runs every half second forever.
        if (Read().valid)
        {
            g_lostAtMs = 0;
            return false;
        }
        if (g_recoverTriesLeft <= 0) return false;

        // Three seconds after it stops answering, not the instant it does. A
        // load frees the world and the actor set is full of pointers into it,
        // and reading those while the game is still handing pages back is what
        // took the session down in a hundred and twenty.
        const uint32_t now = GetTickCount();
        if (!g_lostAtMs) { g_lostAtMs = now; return false; }
        if (now - g_lostAtMs < 3000) return false;

        // The pools rather than the set. The set only holds entities whose
        // world position could be worked out, that needs the player, and the
        // player is what this is looking for.
        static uintptr_t pool[1536];
        const int n = gs::actors::Offered(pool, 1536);
        uintptr_t found = 0;
        int looked = 0;

        __try
        {
            for (int i = 0; i < n && !found; ++i)
            {
                const uintptr_t comps = Deref(pool[i] + kOff_Ent_Comps);
                if (!comps) continue;
                ++looked;
                bool isPlayer = false;
                for (uintptr_t off = 0; off < kComps_SlotsEnd && !isPlayer; off += 8)
                {
                    const uintptr_t c = Deref(comps + off);
                    if (!c) continue;
                    const char* name = NameOf(c);
                    isPlayer = name && strstr(name, "ClientUserLoginActorComponent") != nullptr;
                }
                if (!isPlayer) continue;
                // Tried where it stands. The frame thread reads the stored one
                // several times a second and must never see a candidate that
                // has not answered yet.
                float v[3]{}, w[7]{};
                uintptr_t actor = 0, tf = 0;
                uint32_t parent = 0;
                if (!Walk(pool[i], v, w, &actor, &tf, &parent, true)) continue;
                if (!std::isfinite(w[0]) || !std::isfinite(w[2])) continue;
                found = pool[i];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // The whole point of the guard, and of counting them: reading a
            // page the game is handing back can fail however carefully it is
            // probed first, and a search that keeps faulting is a search that
            // should stop rather than keep rolling the dice.
            g_lostAtMs = now;
            if (--g_recoverTriesLeft <= 0)
                GS_LOG_ERR("[player] the search through the actor manager faulted eight times; it "
                           "is off for the rest of this session and the heap sweep is the only "
                           "way back");
            return false;
        }

        if (!found) return false;
        g_actor.store(found);
        g_lostAtMs = 0;
        GS_LOG_OK("[player] found again through the actor manager: the player is at 0x%p, %d of %d "
                  "entities looked at, and no heap walk. The flash still waits for the sweep, "
                  "because the component it needs is not on him.",
                  reinterpret_cast<void*>(found), looked, n);
        return true;
    }

    Pos Read()
    {
        Pos p;
        // The component when there is one, the actor when there is not. After
        // a load the mod has the actor from the pools long before the sweep
        // hands the component back, and everything except the flash works off
        // the actor alone.
        auto from = reinterpret_cast<uintptr_t>(g_comp.load());
        bool fromActor = false;
        if (!from)
        {
            from = g_actor.load();
            fromActor = true;
        }
        if (!from) return p;
        const uintptr_t comp = from;

        float v[3]{}, w[7]{};
        uintptr_t actor = 0, tf = 0;
        uint32_t parent = 0;
        if (!Walk(comp, v, w, &actor, &tf, &parent, fromActor))
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
            GS_LOG_OK("[player] local (%.3f, %.3f, %.3f) world (%.3f, %.3f, %.3f) origin (%.1f, %.1f, %.1f), parent id 0x%08X",
                      v[0], v[1], v[2], w[0], w[1], w[2], w[0] - v[0], w[1] - v[1], w[2] - v[2], parent);
        }

        const bool worldOk = std::isfinite(w[0]) && std::isfinite(w[1]) && std::isfinite(w[2]) &&
                             std::fabs(w[0]) + std::fabs(w[1]) + std::fabs(w[2]) < 1.0e6f;
        if (!worldOk) return p;
        p.lx = v[0]; p.ly = v[1]; p.lz = v[2];
        p.x = w[0]; p.y = w[1]; p.z = w[2];
        p.ox = w[0] - v[0]; p.oy = w[1] - v[1]; p.oz = w[2] - v[2];
        memcpy(p.q, w + 3, 16);
        p.valid = true;
        {
            const char* on = NameOf(actor);
            g_ownerIsBody.store(on && strstr(on, "ClientChildOnlyInGameActor") != nullptr);
        }
        if (g_actor.load() != actor)
        {
            g_actor.store(actor);
            gs::aim::SetPlayerActor(actor);
        }
        if (comp) gs::aim::SetSpecialComponent(comp);

        // Look through the actor's components once per actor, and again every
        // second while the flash's own component is still missing.
        //
        // The test here used to be the actor pointer changing. The recovery
        // through the actor manager stores that pointer itself, so after a
        // recovery this never ran: the flash spent the rest of the session
        // with no detect component and marked nothing, which is what happened
        // in the first 1.1.0 session from the moment the world loaded.
        static std::atomic<uintptr_t> g_scannedActor{0};
        static std::atomic<uint32_t> g_scannedMs{0};
        const uint32_t nowMs = GetTickCount();
        const bool actorIsNew = g_scannedActor.load() != actor;
        const bool stillMissing = g_detect.load() == 0;
        if (actorIsNew || (stillMissing && nowMs - g_scannedMs.load() > 1000))
        {
            g_scannedActor.store(actor);
            g_scannedMs.store(nowMs);
            // A new actor means the old world's components are gone, so the
            // one held is dropped before looking rather than kept as a
            // pointer into whatever the game has put there since.
            if (actorIsNew)
            {
                g_detect.store(0);
                gs::aim::SetDetectComponent(0);
            }
            // The flash's own component sits in the same block. Found by
            // name, because slots can move between patches and names do not.
            const uintptr_t comps = Deref(actor + kOff_Ent_Comps);
            for (uintptr_t off = 0; comps && off < kComps_SlotsEnd; off += 8)
            {
                const uintptr_t c = Deref(comps + off);
                const char* n = c ? NameOf(c) : nullptr;
                if (n && strstr(n, "ClientDetectActorComponent"))
                {
                    g_detect.store(c);
                    gs::aim::SetDetectComponent(c);
                    GS_LOG_OK("[player] detect component at block+0x%llX -> 0x%p",
                              static_cast<unsigned long long>(off), reinterpret_cast<void*>(c));
                }
                if (n && strstr(n, "ClientCharacterControlActorComponent"))
                {
                    g_charctl.store(c);
                    GS_LOG_OK("[player] character control at block+0x%llX -> 0x%p",
                              static_cast<unsigned long long>(off), reinterpret_cast<void*>(c));
                }
            }
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_last = p;
        return p;
    }

    Pos Last()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_last;
    }

    uintptr_t Actor() { return g_actor.load(); }
    uintptr_t DetectComponent() { return g_detect.load(); }
    uintptr_t CharacterControlComponent() { return g_charctl.load(); }
    bool OwnerIsPlayedBody() { return g_ownerIsBody.load(); }
}
