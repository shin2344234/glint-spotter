#include "game/realpin.h"

#include <Windows.h>
#include <cstring>

#include "core/log.h"
#include "game/pinmodel.h"
#include "game/player.h"
#include "game/rtti.h"
#include "game/signatures.h"
#include "game/typescan.h"

namespace
{
    using CreateFn = void (*)(void* submodule, void* unused, const float* pos,
                              const uint8_t* b1, const uint8_t* b2, uint8_t second);

    uintptr_t g_base = 0;
    size_t g_size = 0;
    bool g_checked = false;
    bool g_bytesOk = false;

    bool Base()
    {
        if (g_base) return true;
        return gs::typescan::ModuleRange(g_base, g_size);
    }

    // The prologue has to be the one the analysis read. Everything else in
    // this file guards against the game not being ready; this guards against
    // the game not being this build.
    bool BytesMatch()
    {
        if (g_checked) return g_bytesOk;
        g_checked = true;
        if (!Base()) return false;
        __try
        {
            const auto* create = reinterpret_cast<const uint8_t*>(g_base + gs::sig::kPinCreate);
            const auto* remove = reinterpret_cast<const uint8_t*>(g_base + gs::sig::kPinServerRemove);
            if (!gs::rtti::Readable(create, sizeof(gs::sig::kPinCreatePrologue))) return false;
            if (!gs::rtti::Readable(remove, sizeof(gs::sig::kPinRemovePrologue))) return false;
            g_bytesOk =
                memcmp(create, gs::sig::kPinCreatePrologue, sizeof(gs::sig::kPinCreatePrologue)) == 0 &&
                memcmp(remove, gs::sig::kPinRemovePrologue, sizeof(gs::sig::kPinRemovePrologue)) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_bytesOk = false;
        }
        if (!g_bytesOk)
            GS_LOG_ERR("[real] the marker calls at +0x%llX and +0x%llX do not start with the bytes this "
                       "build was written against, so the game has been patched. Pins stay drawn and "
                       "undeletable until the addresses are found again.",
                       static_cast<unsigned long long>(gs::sig::kPinCreate),
                       static_cast<unsigned long long>(gs::sig::kPinServerRemove));
        else
            GS_LOG_OK("[real] the game's own marker calls are where this build expects them");
        return g_bytesOk;
    }

    // The two gates the game puts in front of its own call, at the call site
    // rather than inside it: a byte one deep in whatever hangs at actor+0x88,
    // and a byte at actor+0x96. Neither was chased to a meaning. What matters
    // is that the game refuses to place a marker when either is wrong, and a
    // record pushed past a refusal is a record the rest of the game does not
    // expect.
    bool ActorAllows()
    {
        const uintptr_t actor = gs::player::Actor();
        if (!actor) return false;
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(actor + 0x88), 8)) return false;
            const uintptr_t sub = *reinterpret_cast<const uintptr_t*>(actor + 0x88);
            if (sub < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(sub), 2))
                return false;
            if (*reinterpret_cast<const uint8_t*>(sub + 1) != 1) return false;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(actor + 0x96), 1)) return false;
            return *reinterpret_cast<const uint8_t*>(actor + 0x96) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The create dereferences the submodule's owner without checking it, and
    // then that owner's first member, so both have to be there before the call
    // is worth making. The game never sees a null one; the mod might, half a
    // second after a load.
    bool Chain(uintptr_t* outSub)
    {
        const uintptr_t sub = gs::pinmodel::Submodule();
        if (!sub) return false;
        __try
        {
            const uintptr_t owner =
                *reinterpret_cast<const uintptr_t*>(sub + gs::sig::kOff_Pin_Owner);
            if (owner < 0x10000) return false;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(owner + 8), 8)) return false;
            const uintptr_t held = *reinterpret_cast<const uintptr_t*>(owner + 8);
            if (held < 0x10000) return false;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(held), 0x10)) return false;
            *outSub = sub;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool OnlineFlag(bool* out)
    {
        if (!Base()) return false;
        __try
        {
            const auto* p = reinterpret_cast<const uint8_t*>(g_base + gs::sig::kPinOnlineFlag);
            if (!gs::rtti::Readable(p, 1)) return false;
            *out = *p != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The last record's id, which is the one the create just appended.
    bool LastId(int64_t* out)
    {
        const gs::pinmodel::List l = gs::pinmodel::Read(gs::sig::kPinListKind);
        if (!l.ok || l.count == 0) return false;
        __try
        {
            const auto* r = reinterpret_cast<const uint8_t*>(l.data) +
                            static_cast<size_t>(l.count - 1) * gs::sig::kPinRecord;
            memcpy(out, r, 8);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

namespace gs::realpin
{
    bool Ready(const char** why)
    {
        static const char* kNoBytes = "the game has been patched away from these addresses";
        static const char* kNoChain = "the player's marker list has not been found yet";
        static const char* kNoActor = "the game itself would refuse to place a marker right now";
        if (!BytesMatch()) { if (why) *why = kNoBytes; return false; }
        uintptr_t sub = 0;
        if (!Chain(&sub)) { if (why) *why = kNoChain; return false; }
        if (!ActorAllows()) { if (why) *why = kNoActor; return false; }
        if (why) *why = "";
        return true;
    }

    int Count()
    {
        const gs::pinmodel::List l = gs::pinmodel::Read(gs::sig::kPinListKind);
        return l.ok ? static_cast<int>(l.count) : -1;
    }

    int64_t Place(float x, float z)
    {
        const char* why = "";
        if (!Ready(&why))
        {
            GS_LOG("[real] not placing a real marker: %s", why);
            return 0;
        }
        uintptr_t sub = 0;
        if (!Chain(&sub)) return 0;

        const int before = Count();
        // Height zero, and the two style bytes the game passed for the marker
        // Seth placed by hand: the icon that came back carried a zero and a
        // one in the two argument slots these end up in. The create refuses
        // anything from 0x0E and 0x0F up, so both are well inside.
        float pos[3] = {x, 0.0f, z};
        uint8_t b1 = 0, b2 = 1;
        const auto fn = reinterpret_cast<CreateFn>(g_base + gs::sig::kPinCreate);

        GS_LOG("[real] asking the game for a marker at (%.1f, %.1f); it is holding %d",
               x, z, before);
        __try
        {
            fn(reinterpret_cast<void*>(sub), nullptr, pos, &b1, &b2, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GS_LOG_ERR("[real] the create call faulted. Nothing else will call it this session.");
            g_bytesOk = false;
            return 0;
        }

        const int after = Count();
        int64_t id = 0;
        if (after > before && LastId(&id))
        {
            GS_LOG_OK("[real] the game holds %d marker(s) now; the new one is id %lld",
                      after, static_cast<long long>(id));
            return id;
        }
        if (after == before && before > 0 && LastId(&id))
        {
            // At the cap the game drops its oldest to make room, so the count
            // does not move and the record is still ours.
            GS_LOG_OK("[real] the list was full at %d, so the game dropped its oldest; the new one "
                      "is id %lld", after, static_cast<long long>(id));
            return id;
        }
        GS_LOG_ERR("[real] the call returned but the list still holds %d. Falling back to a drawn pin.",
                   after);
        return 0;
    }

    void LogState(const char* why)
    {
        bool online = false;
        if (OnlineFlag(&online))
            GS_LOG("[real] %s: the game keeps %s markers here", why, online ? "500" : "15");
        for (int k = 0; k <= 1; ++k)
        {
            const gs::pinmodel::List l = gs::pinmodel::Read(k);
            if (!l.ok)
            {
                GS_LOG("[real]   list %d does not read", k);
                continue;
            }
            GS_LOG("[real]   list %d (%s): %u record(s)", k, k == 0 ? "markers" : "traced", l.count);
            __try
            {
                for (uint32_t i = 0; i < l.count && i < 20; ++i)
                {
                    const auto* r = reinterpret_cast<const uint8_t*>(l.data) +
                                    static_cast<size_t>(i) * gs::sig::kPinRecord;
                    int64_t id; float px, py, pz;
                    memcpy(&id, r, 8);
                    memcpy(&px, r + 8, 4); memcpy(&py, r + 12, 4); memcpy(&pz, r + 16, 4);
                    GS_LOG("[real]     [%u] id %lld at (%.1f, %.1f, %.1f) style %02X %02X",
                           i, static_cast<long long>(id), px, py, pz, r[20], r[21]);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                GS_LOG_ERR("[real]   a record faulted while printing");
            }
        }
    }
}
