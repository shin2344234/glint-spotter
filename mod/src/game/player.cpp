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
#include "game/camera.h"

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
    // For the played body when it is not the player actor, Master Looter's
    // signatures.h again: the entity id, the type descriptor whose second
    // byte is the type tag, and the status component's second category byte.
    constexpr uintptr_t kOff_Ent_Eid       = 0x60;
    constexpr uintptr_t kOff_Ent_TypeInfo  = 0x88;
    constexpr uintptr_t kOff_Status_Cat2   = 0x2F0;

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
    // The body being played when it is a second character's, and its id.
    std::atomic<uintptr_t> g_body{0};
    std::atomic<uint32_t> g_bodyEid{0};
    std::atomic<uint32_t> g_bodyLookMs{0};

    using gs::rtti::Deref;

    const char* NameOf(uintptr_t obj)
    {
        const char* n = gs::rtti::NameOf(obj);
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

    // The flash's own component, if it is in the block at all. Every session
    // so far found it by walking the heap, and the block was only ever read
    // to +0x80; the transform sits at +0x1A0 and knowledge at +0x150, so it
    // runs well past that. This looks as far as +0x400 for a special mode
    // component whose owner is this actor. Nothing but a match on both is
    // taken, and a read past the block's real end is guarded. If it is
    // there, a load stops costing a walk.
    uintptr_t FindSpecialInBlock(uintptr_t actor, uintptr_t comps, uintptr_t* atOut)
    {
        __try
        {
            for (uintptr_t off = 0; off < 0x400; off += 8)
            {
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(comps + off), 8)) return 0;
                const uintptr_t c = *reinterpret_cast<const uintptr_t*>(comps + off);
                if (c < 0x10000 || (c & 7) != 0) continue;
                const char* n = NameOf(c);
                if (!n || !strstr(n, "ClientSpecialModeActorComponent")) continue;
                if (Deref(c + kOff_Comp_Owner) != actor) continue;
                *atOut = off;
                return c;
            }
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
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

    // While a world loads every position reads near (0, 1000, 0), and the
    // pinning side refuses anything within a hundred of the origin.
    bool OnPlaceholder(const float* w) { return std::fabs(w[0]) + std::fabs(w[2]) < 100.0f; }

    // These two run on every entity in the pools, and B0 is every world
    // object, so the handler is the guard and there is no VirtualQuery.
    // Deref asks one per call, and actors.cpp measured what a thousand of
    // those a pass cost.
    uint32_t EidAt(uintptr_t e)
    {
        __try
        {
            if (e < 0x10000 || (e & 7) != 0) return 0;
            return *reinterpret_cast<const uint32_t*>(e + kOff_Ent_Eid);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    uint8_t TypeTag(uintptr_t e)
    {
        __try
        {
            if (e < 0x10000 || (e & 7) != 0) return 0xFF;
            const uintptr_t ti = *reinterpret_cast<const uintptr_t*>(e + kOff_Ent_TypeInfo);
            if (ti < 0x10000) return 0xFF;
            return *reinterpret_cast<const uint8_t*>(ti + 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFF; }
    }

    uint8_t Cat2(uintptr_t e)
    {
        __try
        {
            const uintptr_t comps = Deref(e + kOff_Ent_Comps);
            for (uintptr_t off = 0; comps && off < kComps_SlotsEnd; off += 8)
            {
                const uintptr_t c = Deref(comps + off);
                const char* n = c ? NameOf(c) : nullptr;
                if (!n || !strstr(n, "ClientStatusActorComponent")) continue;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(c + kOff_Status_Cat2), 1)) return 0;
                return *reinterpret_cast<const uint8_t*>(c + kOff_Status_Cat2);
            }
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    float Dist(const float* a, const float* b)
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // The camera follows whichever body is being played, and keeps that
    // body's world position beside its pivot (camera.h). That makes it the
    // referee between the player actor and a second character's body: on 4
    // November AsunaKaoru's actor stood still where Kliff last was for the
    // whole of her time as Damiane and as Oongka, off the placeholder, while
    // the camera's world copy moved with the body she played. The pivot sits
    // about a metre and a half above the feet, so "agrees" is 40 m, the same
    // margin the view ray uses.
    constexpr float kAgree = 40.0f;

    // Read at most every quarter second, because the camera's own read asks
    // VirtualQuery once and Read() runs from every thread.
    std::mutex g_camMutex;
    uint32_t g_camMs = 0;
    float g_camWorld[3]{};
    bool g_camOk = false;

    bool CameraWorld(float* out)
    {
        std::lock_guard<std::mutex> lock(g_camMutex);
        const uint32_t now = GetTickCount();
        if (!g_camMs || now - g_camMs >= 250)
        {
            g_camMs = now ? now : 1;
            const gs::camera::Pose cam = gs::camera::Read();
            g_camOk = cam.valid && cam.worldValid && !OnPlaceholder(cam.world);
            if (g_camOk) memcpy(g_camWorld, cam.world, sizeof(g_camWorld));
        }
        if (g_camOk) memcpy(out, g_camWorld, sizeof(g_camWorld));
        return g_camOk;
    }

    // A body from the pools that reads somewhere real. With `eid` set it is
    // that entity; with it zero it is any body wearing the pair Master
    // Looter found only on the one being played, type tag 04 with status
    // category 0E, in two Damiane logs from two machines. With `nearTo` set,
    // only a body within kAgree of it counts, and the nearest one wins.
    uintptr_t FindBody(uint32_t eid, const float* nearTo, int* seenOut)
    {
        static uintptr_t pool[1536];
        const int n = gs::actors::Offered(pool, 1536);
        uintptr_t found = 0;
        float best = 0;
        int seen = 0;
        for (int i = 0; i < n; ++i)
        {
            const uint32_t id = EidAt(pool[i]);
            if ((id >> 24) != 0xB0) continue;
            if (eid ? id != eid : (TypeTag(pool[i]) != 0x04 || Cat2(pool[i]) != 0x0E)) continue;
            float v[3]{}, w[7]{};
            uintptr_t actor = 0, tf = 0;
            uint32_t parent = 0;
            if (!Walk(pool[i], v, w, &actor, &tf, &parent, true)) continue;
            if (!std::isfinite(w[0]) || !std::isfinite(w[1]) || !std::isfinite(w[2]) || OnPlaceholder(w))
                continue;
            ++seen;
            const float d = nearTo ? Dist(w, nearTo) : 0.0f;
            if (nearTo && d > kAgree) continue;
            if (!found || d < best) { found = pool[i]; best = d; }
        }
        *seenOut = seen;
        return found;
    }

    // Where positions come from. Decided twice a second and used in between,
    // so the reads every thread makes cost what they always did.
    enum Source : int { kFromActor = 0, kFromBody = 1, kFromShifted = 2 };
    std::atomic<int> g_source{kFromActor};
    std::atomic<uint32_t> g_pickMs{0};
    std::atomic<uint32_t> g_tagLookMs{0};
    std::mutex g_pick;
    // For kFromShifted, the sub-level origin in whole kilometres. Every
    // origin seen in a log has been a multiple of 1000 with y at 0:
    // (-9000, 0, -4000), (-11000, 0, -5000), (-12000, 0, -5000).
    std::atomic<int> g_shiftX{0}, g_shiftZ{0};
    bool g_disagreeSaid = false;

    // The body held, read where it stands. False when the block is gone or
    // somebody else's, or it reads the placeholder.
    bool ReadHeldBody(float* v, float* w)
    {
        const uintptr_t b = g_body.load();
        if (!b) return false;
        uintptr_t actor = 0, tf = 0;
        uint32_t parent = 0;
        if (EidAt(b) == g_bodyEid.load() && Walk(b, v, w, &actor, &tf, &parent, true) &&
            std::isfinite(w[0]) && std::isfinite(w[1]) && std::isfinite(w[2]) && !OnPlaceholder(w))
            return true;
        g_body.store(0);
        return false;
    }

    // Look for the played body in the pools. `eid` first when there is one;
    // the tag pair only when `tagToo`. Holds g_body and g_bodyEid on success.
    bool SearchBody(uint32_t eid, bool tagToo, const float* nearTo, float* v, float* w)
    {
        const uint32_t now = GetTickCount();
        int seen = 0;
        uintptr_t found = 0;
        const char* how = "the flash names it";
        if (eid && now - g_bodyLookMs.load() >= 1000)
        {
            g_bodyLookMs.store(now);
            found = FindBody(eid, nearTo, &seen);
            if (!found)
            {
                static uint32_t s_missSaid = 0;
                if (s_missSaid != eid)
                {
                    s_missSaid = eid;
                    GS_LOG("[player] the flash names the body %08X and it is not in the actor manager's "
                           "pools%s yet; positions wait for it", eid, nearTo ? " beside the camera" : "");
                }
            }
        }
        if (!found && tagToo && now - g_tagLookMs.load() >= 5000)
        {
            g_tagLookMs.store(now);
            found = FindBody(0, nearTo, &seen);
            how = "it wears the played body's type tag 04 and status category 0E";
        }
        if (!found) return false;

        const uint32_t id = EidAt(found);
        uintptr_t actor = 0, tf = 0;
        uint32_t parent = 0;
        float bv[3]{}, bw[7]{};
        if (!Walk(found, bv, bw, &actor, &tf, &parent, true)) return false;
        g_body.store(found);
        if (g_bodyEid.exchange(id) != id)
            GS_LOG_OK("[player] a second character is being played. The player actor stays behind, "
                      "so positions come from the body %08X at 0x%p, world (%.1f, %.1f, %.1f), "
                      "because %s (%d matching in the pools)",
                      id, reinterpret_cast<void*>(found), bw[0], bw[1], bw[2], how, seen);
        memcpy(v, bv, sizeof(bv));
        memcpy(w, bw, sizeof(bw));
        return true;
    }

    void SaySource(int was, int now, const float* w)
    {
        if (was == now) return;
        if (now == kFromActor && was == kFromBody)
            GS_LOG("[player] the player actor reads (%.1f, %.1f, %.1f) again, so positions come from it "
                   "and not the body %08X", w[0], w[1], w[2], g_bodyEid.load());
        else if (now == kFromShifted)
            GS_LOG("[player] the player actor's world position is wrong and its local one is right, so "
                   "positions are local plus (%d, 0, %d), the origin the camera puts it in; world "
                   "(%.1f, %.1f, %.1f)", g_shiftX.load() * 1000, g_shiftZ.load() * 1000, w[0], w[1], w[2]);
        else if (now == kFromActor && was == kFromShifted)
            GS_LOG("[player] the player actor's world position agrees with the camera again, (%.1f, %.1f, %.1f)",
                   w[0], w[1], w[2]);
    }

    // Damiane and Oongka, and Kliff's world position after a load.
    //
    // The player actor carries the login, detect and flash components, and
    // while Damiane or Oongka is played the body that moves is a second
    // entity with a B0 id. 1.1.38 used that body only while the actor read
    // the loading placeholder, from AsunaKaoru's 2 November log. Her 4
    // November log on 1.1.38 had the actor off the placeholder, standing
    // still at (-12083.6, 866.8, -4884.1) where Kliff had been, so the body
    // was never looked for: every press as Damiane cast from that spot and
    // pinned glints 279 to 710 m off, and as Oongka nothing was on the line
    // at all. The same log had Kliff's world position reading (0.4, 0.6,
    // 0.8) for two minutes after a load while his local one was right, and
    // one press pinned a glint 3.7 km away from it.
    //
    // So the choice is now made against the camera: the actor when it
    // agrees, the actor's local position moved by a whole-kilometre origin
    // when that agrees, and the body the flash named, or the one wearing
    // the played body's tags, when it agrees. With no camera, or nothing
    // agreeing, the flash's tail decides: a B0 id means that body, an A0 id
    // means the actor, and the placeholder means a body known before.
    void Choose(const float* av, const float* aw, float* v, float* w)
    {
        const int was = g_source.load();
        const uint32_t now = GetTickCount();
        std::unique_lock<std::mutex> lock(g_pick, std::defer_lock);
        const bool decide = now - g_pickMs.load() >= 500 && lock.try_lock();
        if (!decide)
        {
            if (was == kFromBody && ReadHeldBody(v, w)) return;
            // Not across a load: the local position goes to the placeholder
            // as well, and moved by an origin it would look like a place.
            if (was == kFromShifted && !OnPlaceholder(av))
            {
                v[0] = av[0]; v[1] = av[1]; v[2] = av[2];
                memcpy(w, aw, 7 * sizeof(float));
                w[0] = av[0] + g_shiftX.load() * 1000.0f;
                w[1] = av[1];
                w[2] = av[2] + g_shiftZ.load() * 1000.0f;
                return;
            }
            memcpy(v, av, 3 * sizeof(float));
            memcpy(w, aw, 7 * sizeof(float));
            return;
        }
        g_pickMs.store(now ? now : 1);

        uint16_t m1 = 0, m2 = 0;
        uint32_t t1 = 0, t2 = 0;
        uint32_t tailBody = 0;
        bool tailActor = false;
        if (gs::aim::ModeRecords(&m1, &m2, &t1, &t2))
        {
            if ((t1 >> 24) == 0xB0) tailBody = t1;
            else if ((t1 >> 24) == 0xA0) tailActor = true;
        }
        if (tailBody && tailBody != g_bodyEid.load()) g_body.store(0);
        const uint32_t eid = tailBody ? tailBody : g_bodyEid.load();
        const bool actorReal = !OnPlaceholder(aw);

        float cam[3]{};
        const bool haveCam = CameraWorld(cam);
        int pick = -1;
        float bv[3]{}, bw[7]{};
        if (haveCam)
        {
            if (actorReal && Dist(aw, cam) <= kAgree) pick = kFromActor;
            if (pick < 0 && !OnPlaceholder(av))
            {
                const int sx = static_cast<int>(std::lround((cam[0] - av[0]) / 1000.0f));
                const int sz = static_cast<int>(std::lround((cam[2] - av[2]) / 1000.0f));
                const float sw[3] = {av[0] + sx * 1000.0f, av[1], av[2] + sz * 1000.0f};
                if (Dist(sw, cam) <= kAgree)
                {
                    g_shiftX.store(sx);
                    g_shiftZ.store(sz);
                    pick = kFromShifted;
                }
            }
            if (pick < 0)
            {
                if (ReadHeldBody(bv, bw) && Dist(bw, cam) <= kAgree) pick = kFromBody;
                else if (SearchBody(eid, true, cam, bv, bw)) pick = kFromBody;
            }
            if (pick >= 0) g_disagreeSaid = false;
            else if (!g_disagreeSaid)
            {
                g_disagreeSaid = true;
                GS_LOG("[player] the camera puts the played body at (%.1f, %.1f, %.1f), and neither the player "
                       "actor at (%.1f, %.1f, %.1f) nor any second body agrees; the flash's tail %08X decides",
                       cam[0], cam[1], cam[2], aw[0], aw[1], aw[2], t1);
            }
        }
        if (pick < 0)
        {
            // A body only on the flash's word or with the actor on the
            // placeholder, and the tag pair only for a body known before
            // that has gone, the way 1.1.38 did it.
            pick = kFromActor;
            if (!tailActor && (tailBody || !actorReal) && eid &&
                (ReadHeldBody(bv, bw) || SearchBody(eid, !tailBody, nullptr, bv, bw)))
                pick = kFromBody;
        }

        if (pick == kFromBody)
        {
            memcpy(v, bv, sizeof(bv));
            memcpy(w, bw, sizeof(bw));
        }
        else
        {
            memcpy(v, av, 3 * sizeof(float));
            memcpy(w, aw, 7 * sizeof(float));
            if (pick == kFromShifted)
            {
                w[0] = av[0] + g_shiftX.load() * 1000.0f;
                w[1] = av[1];
                w[2] = av[2] + g_shiftZ.load() * 1000.0f;
            }
        }
        g_source.store(pick);
        SaySource(was, pick, w);
        // Kliff named by the flash and read from the actor: whatever body was
        // held belongs to a character he is not playing any more.
        if (tailActor && pick != kFromBody)
        {
            g_body.store(0);
            g_bodyEid.store(0);
        }
    }
}

namespace gs::player
{
    void SetSpecialComponent(void* comp)
    {
        g_comp.store(comp);
        // The aim module reads the flash flag out of this object, so it hears
        // about the null as well as the pointer. It used to hear only the
        // pointer, and went on reading a freed one after every load.
        gs::aim::SetSpecialComponent(reinterpret_cast<uintptr_t>(comp));
    }

    uintptr_t SpecialComponent() { return reinterpret_cast<uintptr_t>(g_comp.load()); }

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
        // The component the walk was starting from belongs to the world that
        // was just thrown away, and Read() prefers it to the actor for as
        // long as it is set. Dropped here, the moment there is a new body to
        // read from, rather than on the worker's next recheck, which can be
        // two minutes off. The worker sees the new body and goes looking.
        // Only for a new body: the same one answering again means the walk
        // failed for a reason the component shares, and dropping it would
        // buy nothing but a walk.
        if (found != g_actor.load())
        {
            if (void* old = g_comp.load())
            {
                GS_LOG("[player] the special mode component at 0x%p stopped answering and the "
                       "manager has a new body, so it is dropped; the flash waits for it to be "
                       "found again", old);
                SetSpecialComponent(nullptr);
            }
        }
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
        // The actor, a second character's body, or the actor's local
        // position in the origin the camera names; Choose says which.
        // Everything below except the position stays on the actor.
        {
            float av[3] = {v[0], v[1], v[2]}, aw[7];
            memcpy(aw, w, sizeof(aw));
            Choose(av, aw, v, w);
        }
        p.lx = v[0]; p.ly = v[1]; p.lz = v[2];
        p.x = w[0]; p.y = w[1]; p.z = w[2];
        p.ox = w[0] - v[0]; p.oy = w[1] - v[1]; p.oz = w[2] - v[2];
        memcpy(p.q, w + 3, 16);
        p.valid = true;
        {
            const char* on = NameOf(actor);
            g_ownerIsBody.store(on && strstr(on, "ClientChildOnlyInGameActor") != nullptr);
        }
        // The aim module's copy every time, not only when the pointer here
        // changes. Recovery through the actor manager stores it here itself,
        // so the compare never fired after a load and the aim went on taking
        // the old body for the player.
        g_actor.store(actor);
        gs::aim::SetPlayerActor(actor);

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
        // Looks into the block for the flash's own component, per actor.
        static std::atomic<int> g_blockLooksLeft{0};
        const uint32_t nowMs = GetTickCount();
        const bool actorIsNew = g_scannedActor.load() != actor;
        const bool stillMissing = g_detect.load() == 0 ||
                                  (!g_comp.load() && g_blockLooksLeft.load() > 0);
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
                g_blockLooksLeft.store(8);
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
            // And the flash's own component, in case it is in here as well.
            // Eight looks a second apart, then the walk is the only way.
            if (!g_comp.load() && comps && g_blockLooksLeft.load() > 0)
            {
                const int left = g_blockLooksLeft.load() - 1;
                g_blockLooksLeft.store(left);
                uintptr_t at = 0;
                const uintptr_t sp = FindSpecialInBlock(actor, comps, &at);
                if (sp)
                {
                    GS_LOG_OK("[player] the special mode component is in the player's own block at "
                              "+0x%llX -> 0x%p, so the flash needs no walk",
                              static_cast<unsigned long long>(at), reinterpret_cast<void*>(sp));
                    SetSpecialComponent(reinterpret_cast<void*>(sp));
                }
                else if (left == 0)
                {
                    GS_LOG("[player] the special mode component is not in the player's block within "
                           "0x400 bytes; the flash waits for the walk");
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
