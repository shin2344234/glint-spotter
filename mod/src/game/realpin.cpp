#include "game/realpin.h"

#include <Windows.h>
#include <intrin.h>
#include <cstring>

#include "core/log.h"
#include "game/pinmodel.h"
#include "game/player.h"
#include "game/rtti.h"
#include "game/signatures.h"
#include "game/typescan.h"
#include "hook/inlinehook.h"

namespace
{
    using CreateFn = void (*)(void* submodule, void* unused, const float* pos,
                              const uint8_t* b1, const uint8_t* b2, uint8_t second);
    using AddedFn = void (*)(void* root, int64_t id, const float* pos,
                             uint8_t b1, uint8_t b2, uint8_t b3);
    using ServerRemoveFn = void (*)(void* submodule, int64_t id, uint8_t second);

    gs::inlinehook::Hook g_createHook;
    gs::inlinehook::Hook g_removeHook;
    int g_spyLogsLeft = 30;

    uintptr_t g_base = 0;
    size_t g_size = 0;
    bool g_checked = false;
    bool g_bytesOk = false;

    // A call that raises is still a call that wrote the record, so the reason
    // to stop is a run of them rather than one. Three, and then the drawn pin
    // is all that is left.
    int g_faultsLeft = 3;
    unsigned long g_faultCode = 0;
    uintptr_t g_faultAt = 0;

    // Where it died, which the old handler threw away. The address is the
    // instruction that raised, so subtracting the module base names the line
    // in the disassembly.
    int FaultFilter(EXCEPTION_POINTERS* ep)
    {
        __try
        {
            g_faultCode = ep->ExceptionRecord->ExceptionCode;
            g_faultAt = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_faultCode = 0;
            g_faultAt = 0;
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

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
            const auto* added = reinterpret_cast<const uint8_t*>(g_base + gs::sig::kMarkerAdded);
            if (!gs::rtti::Readable(added, sizeof(gs::sig::kMarkerAddedPrologue))) return false;
            g_bytesOk =
                memcmp(create, gs::sig::kPinCreatePrologue, sizeof(gs::sig::kPinCreatePrologue)) == 0 &&
                memcmp(remove, gs::sig::kPinRemovePrologue, sizeof(gs::sig::kPinRemovePrologue)) == 0 &&
                memcmp(added, gs::sig::kMarkerAddedPrologue, sizeof(gs::sig::kMarkerAddedPrologue)) == 0;
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

    // The last record, which is the one the create just appended.
    bool LastRecord(int64_t* id, float* x, float* z)
    {
        const gs::pinmodel::List l = gs::pinmodel::Read(gs::sig::kPinListKind);
        if (!l.ok || l.count == 0) return false;
        __try
        {
            const auto* r = reinterpret_cast<const uint8_t*>(l.data) +
                            static_cast<size_t>(l.count - 1) * gs::sig::kPinRecord;
            memcpy(id, r, 8);
            memcpy(x, r + 8, 4);
            memcpy(z, r + 16, 4);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The record we asked for, rather than any record. Two marks in a row at
    // the same place would otherwise report the first one's id forever.
    bool Landed(float wantX, float wantZ, int64_t* id)
    {
        float gotX = 0.0f, gotZ = 0.0f;
        if (!LastRecord(id, &gotX, &gotZ)) return false;
        const float dx = gotX - wantX, dz = gotZ - wantZ;
        return dx * dx + dz * dz < 1.0f;
    }
}

namespace
{
    // Both detours do the same three things: say who called, say which object,
    // and get out of the way. The object is the whole point. If it is not the
    // one the mod computes off the player, the mod has been writing to the
    // wrong list for four builds and the right one is one pointer away.
    void Say(const char* what, uintptr_t ret, uintptr_t sub)
    {
        if (g_spyLogsLeft <= 0) return;
        --g_spyLogsLeft;
        const uintptr_t mine = gs::pinmodel::Submodule();
        GS_LOG_OK("[mspy] the game called %s on 0x%p, from +0x%08llX. The mod's own submodule is "
                  "0x%p, so this is %s object.", what, reinterpret_cast<void*>(sub),
                  static_cast<unsigned long long>(ret > g_base ? ret - g_base : 0),
                  reinterpret_cast<void*>(mine),
                  (mine && mine == sub) ? "the same" : "a different");
    }

    void DetourCreate(void* sub, void* unused, const float* pos, const uint8_t* b1,
                      const uint8_t* b2, uint8_t second)
    {
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        Say("create", ret, reinterpret_cast<uintptr_t>(sub));
        if (g_spyLogsLeft >= 0)
        {
            __try
            {
                GS_LOG("[mspy]   at (%.1f, %.1f, %.1f), style %u and %u, list %u",
                       pos ? pos[0] : 0.0f, pos ? pos[1] : 0.0f, pos ? pos[2] : 0.0f,
                       b1 ? *b1 : 0u, b2 ? *b2 : 0u, second);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        reinterpret_cast<CreateFn>(g_createHook.trampoline)(sub, unused, pos, b1, b2, second);
    }

    void DetourRemove(void* sub, int64_t id, uint8_t second)
    {
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        Say("remove", ret, reinterpret_cast<uintptr_t>(sub));
        if (g_spyLogsLeft >= 0)
            GS_LOG("[mspy]   id %lld from list %u", static_cast<long long>(id), second);
        reinterpret_cast<ServerRemoveFn>(g_removeHook.trampoline)(sub, id, second);
    }
}

namespace gs::realpin
{
    bool InstallSpy()
    {
        // The check has to run before the bytes are replaced, and it caches,
        // so afterwards it still answers for the game rather than for us.
        if (!BytesMatch()) return false;
        // Fifteen bytes of the create and fourteen of the remove, both read
        // off the disassembly and both nothing but register spills and pushes,
        // so they mean the same thing wherever they are copied to.
        const bool a = gs::inlinehook::Install(
            g_createHook, g_base + gs::sig::kPinCreate, 15,
            reinterpret_cast<void*>(&DetourCreate),
            gs::sig::kPinCreatePrologue, sizeof(gs::sig::kPinCreatePrologue));
        const bool b = gs::inlinehook::Install(
            g_removeHook, g_base + gs::sig::kPinServerRemove, 14,
            reinterpret_cast<void*>(&DetourRemove),
            gs::sig::kPinRemovePrologue, sizeof(gs::sig::kPinRemovePrologue));
        GS_LOG(a && b
               ? "[mspy] both of the game's marker calls are watched. Place one of your own "
                 "markers on the map and the log says which object it went into."
               : "[mspy] the marker calls could not be watched; nothing was patched");
        return a && b;
    }

    void RemoveSpy()
    {
        gs::inlinehook::Remove(g_createHook);
        gs::inlinehook::Remove(g_removeHook);
    }

    bool Ready(const char** why)
    {
        static const char* kNoBytes = "the game has been patched away from these addresses";
        static const char* kNoTries = "the call raised three times, so it is not being made again";
        static const char* kNoChain = "the player's marker list has not been found yet";
        static const char* kNoActor = "the game itself would refuse to place a marker right now";
        if (!BytesMatch()) { if (why) *why = kNoBytes; return false; }
        if (g_faultsLeft <= 0) { if (why) *why = kNoTries; return false; }
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

    bool Place(float x, float z, int64_t* outId)
    {
        *outId = 0;
        const char* why = "";
        if (!Ready(&why))
        {
            GS_LOG("[real] not placing a real marker: %s", why);
            return false;
        }
        uintptr_t sub = 0;
        if (!Chain(&sub)) return false;

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
        bool raised = false;
        g_faultCode = 0;
        g_faultAt = 0;
        __try
        {
            fn(reinterpret_cast<void*>(sub), nullptr, pos, &b1, &b2, 0);
        }
        __except (FaultFilter(GetExceptionInformation()))
        {
            raised = true;
            --g_faultsLeft;
        }

        // The record first, because session a hundred and three had the call
        // raise and the record land anyway: list 0 held it, at the position
        // asked for, with an id of the game's choosing. Whatever raised did so
        // after the append, and an id in hand is worth more than a tidy return
        // path.
        const int after = Count();
        int64_t id = 0;
        const bool landed = Landed(x, z, &id);

        if (raised)
        {
            const uintptr_t rva = (g_faultAt > g_base) ? g_faultAt - g_base : 0;
            GS_LOG_ERR("[real] the call raised 0x%08lX at +0x%08llX, %d attempt(s) left. The list "
                       "holds %d and the record %s.", g_faultCode,
                       static_cast<unsigned long long>(rva), g_faultsLeft, after,
                       landed ? "is there" : "is not");
        }

        if (landed)
        {
            GS_LOG_OK("[real] the game holds %d marker(s); this one is id %lld", after,
                      static_cast<long long>(id));
            *outId = id;
            return true;
        }
        GS_LOG_ERR("[real] no record for this mark; the list holds %d. Falling back to a drawn pin.",
                   after);
        return false;
    }

    bool Draw(void* worldRoot, int64_t id, float x, float z)
    {
        if (!worldRoot || !BytesMatch()) return false;
        // The three bytes the dispatcher hands a subscriber are the record's
        // own bytes at +0x14, +0x15 and +0x16. The first two are what this
        // build writes into the record; the third is whatever the game's
        // create left on its stack, and the icon the spy captured from a
        // hand-placed marker carried a zero and a one, so a zero here matches
        // what the map has already been seen to accept.
        float pos[3] = {x, 0.0f, z};
        const auto fn = reinterpret_cast<AddedFn>(g_base + gs::sig::kMarkerAdded);
        GS_LOG("[real] telling the map about marker id %lld at (%.1f, %.1f)",
               static_cast<long long>(id), x, z);
        g_faultCode = 0;
        g_faultAt = 0;
        __try
        {
            fn(worldRoot, id, pos, 0, 1, 0);
        }
        __except (FaultFilter(GetExceptionInformation()))
        {
            const uintptr_t rva = (g_faultAt > g_base) ? g_faultAt - g_base : 0;
            GS_LOG_ERR("[real] the map's own add handler raised 0x%08lX at +0x%08llX; the icon is "
                       "drawn the old way instead", g_faultCode,
                       static_cast<unsigned long long>(rva));
            return false;
        }
        GS_LOG_OK("[real] the map's add handler returned for id %lld", static_cast<long long>(id));
        return true;
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
