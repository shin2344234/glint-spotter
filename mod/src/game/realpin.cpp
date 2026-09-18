#include "game/realpin.h"

#include <Windows.h>
#include <atomic>
#include <intrin.h>
#include <cstring>

#include "core/log.h"
#include "core/settings.h"
#include "game/pinmodel.h"
#include "game/actors.h"
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
    using UpsertFn = void (*)(void* submodule, int32_t* outError, uint8_t kind, const void* desc);
    using EraseFn = void (*)(void* submodule, int32_t* outError, const int64_t* id, uint8_t kind);

    // The mod's own ids, and the ones the map has asked to be rid of. Eight
    // slots is more presses than anyone makes between two ticks.
    std::atomic<int64_t> g_nextId{gs::sig::kModIdBase};
    std::atomic<int64_t> g_retire[8];

    gs::inlinehook::Hook g_createHook;
    gs::inlinehook::Hook g_removeHook;
    int g_spyLogsLeft = 200;

    // The object the game itself writes markers into, learned the first time
    // the player places one. Until then the mod has only the one hanging off
    // the actor it found, which is a different object and the wrong one.
    std::atomic<uintptr_t> g_gameSub{0};
    bool g_saidWhose = false;


    uintptr_t g_base = 0;
    size_t g_size = 0;
    bool g_checked = false;
    bool g_bytesOk = false;

    // A call that raises is still a call that wrote the record, and session a
    // hundred and seven has it doing exactly that three times out of three.
    // So the count is of calls that produced nothing, not of calls that
    // raised. Three of those in a row and the drawn pin is all that is left.
    int g_faultsLeft = 3;
    bool g_saidWhyItRaises = false;
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
            // The map's marker-added handler used to be checked here as well.
            // Nothing calls it, its prologue is shared by hundreds of functions,
            // and on 2944 where it went is not settled, so it no longer decides
            // whether the two calls below are made.
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
    // Validate whichever object we are about to hand the create. The game's
    // own, once seen, beats the one off the player every time.
    bool Chain(uintptr_t* outSub)
    {
        uintptr_t sub = g_gameSub.load();
        if (!sub) sub = gs::pinmodel::Submodule();
        if (!sub) return false;
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(sub),
                                gs::sig::kOff_Pin_Lists + 40 * 16)) return false;
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

    // The client copy, always. It preferred the server component for three
    // builds, from when the mod wrote its records there, and kept the
    // preference after the writing moved. Everything the mod puts in a list
    // now goes through Mirror, and Mirror writes the client copy, so this has
    // to read the same one or it answers about somebody else's markers.
    gs::pinmodel::List ReadList(int kind)
    {
        return gs::pinmodel::Read(kind);
    }

    // The last record, which is the one the create just appended.
    bool LastRecord(int64_t* id, float* x, float* z)
    {
        const gs::pinmodel::List l = ReadList(gs::sig::kPinListKind);
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

    // Put the same record into the client copy, which is what the map's UI
    // reads. The server list decides whether a removal can succeed; this one
    // decides whether the UI will ask for one at all.
    bool Mirror(int64_t id, float x, float z)
    {
        const uintptr_t client = gs::pinmodel::Submodule();
        if (!client) return false;
        __try
        {
            const auto* code = reinterpret_cast<const uint8_t*>(g_base + gs::sig::kPinUpsert);
            if (!gs::rtti::Readable(code, sizeof(gs::sig::kPinUpsertPrologue))) return false;
            if (memcmp(code, gs::sig::kPinUpsertPrologue,
                       sizeof(gs::sig::kPinUpsertPrologue)) != 0)
            {
                GS_LOG_ERR("[real] the client copy's writer is not where this build expects it");
                return false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        // Twenty-four bytes, the shape both halves use: the id, the position
        // with the height the game writes for a marker, and the two style
        // bytes off the game's own call.
        uint8_t record[gs::sig::kPinRecord]{};
        const float y = 0.0f;
        memcpy(record, &id, 8);
        memcpy(record + 8, &x, 4);
        memcpy(record + 12, &y, 4);
        memcpy(record + 16, &z, 4);
        record[20] = gs::Settings::Get().pinStyle1;
        record[21] = gs::Settings::Get().pinStyle2;
        struct { const void* ptr; int32_t count; int32_t pad; } desc{record, 1, 0};
        int32_t err = -1;

        const auto fn = reinterpret_cast<UpsertFn>(g_base + gs::sig::kPinUpsert);
        __try
        {
            fn(reinterpret_cast<void*>(client), &err, 0, &desc);
        }
        __except (FaultFilter(GetExceptionInformation()))
        {
            const uintptr_t rva = (g_faultAt > g_base) ? g_faultAt - g_base : 0;
            GS_LOG_ERR("[real] the client copy raised 0x%08lX at +0x%08llX", g_faultCode,
                       static_cast<unsigned long long>(rva));
            return false;
        }
        const gs::pinmodel::List l = gs::pinmodel::ReadAt(client, gs::sig::kPinListKind);
        GS_LOG_OK("[real] the client copy took id %lld, status %d, and now holds %u",
                  static_cast<long long>(id), err, l.ok ? l.count : 0u);
        return true;
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
    // Which actor, if any, owns a given marker list. Knowing that is how the
    // bootstrap goes away: today the mod has to watch the player place one
    // marker before it knows where to write, and an actor it can name is an
    // actor it can find on its own next session.
    void NameOwner(uintptr_t sub)
    {
        gs::actors::Entity set[512];
        const int n = gs::actors::Snapshot(set, 512);
        for (int i = 0; i < n; ++i)
        {
            __try
            {
                const uintptr_t comps = *reinterpret_cast<const uintptr_t*>(
                    set[i].ptr + gs::sig::kOff_Actor_Components);
                if (comps < 0x10000) continue;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(
                        comps + gs::sig::kOff_Comp_PinSubmodule), 8)) continue;
                if (*reinterpret_cast<const uintptr_t*>(comps + gs::sig::kOff_Comp_PinSubmodule) != sub)
                    continue;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;
            }
            GS_LOG_OK("[mspy]   that list belongs to eid %08X, actor 0x%p, which is in the set",
                      set[i].eid, reinterpret_cast<void*>(set[i].ptr));
            return;
        }
        GS_LOG("[mspy]   no actor in the set of %d owns that list, so whatever holds it is not "
               "something the pools offer", n);
    }

    void Say(const char* what, uintptr_t ret, uintptr_t sub)
    {
        const uintptr_t mine = gs::pinmodel::Submodule();
        if (sub >= 0x10000 && sub != mine) g_gameSub.store(sub);
        if (g_spyLogsLeft <= 0) return;
        --g_spyLogsLeft;
        GS_LOG_OK("[mspy] the game called %s on 0x%p, from +0x%08llX on thread %lu. The mod's own "
                  "submodule is 0x%p, so this is %s object.", what, reinterpret_cast<void*>(sub),
                  static_cast<unsigned long long>(ret > g_base ? ret - g_base : 0),
                  GetCurrentThreadId(), reinterpret_cast<void*>(mine),
                  (mine && mine == sub) ? "the same" : "a different");
        if (!g_saidWhose)
        {
            g_saidWhose = true;
            const char* cls = nullptr;
            __try
            {
                cls = gs::rtti::VtableClassName(*reinterpret_cast<const void* const*>(sub));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            GS_LOG_OK("[mspy]   it is a %s, and every mark from here on writes into it",
                      cls ? cls : "class with no readable name");
            NameOwner(sub);
        }
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
        // A request for one of the mod's own ids is the map asking for a pin
        // to go. The server will find nothing and do nothing, which is fine,
        // and the work happens on the tick rather than on this thread.
        if (id >= gs::sig::kModIdBase)
        {
            for (auto& slot : g_retire)
            {
                int64_t empty = 0;
                if (slot.compare_exchange_strong(empty, id)) break;
            }
        }
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

    void SetServerSubmodule(void* sub)
    {
        const uintptr_t p = reinterpret_cast<uintptr_t>(sub);
        if (!p || g_gameSub.load()) return;
        g_gameSub.store(p);
        GS_LOG_OK("[real] the sweep found the server's marker component at 0x%p, so a mark works "
                  "without waiting for you to place one first", sub);
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
        if (g_gameSub.load() == 0 && !ActorAllows()) { if (why) *why = kNoActor; return false; }
        if (why) *why = "";
        return true;
    }

    int Count()
    {
        const gs::pinmodel::List l = ReadList(gs::sig::kPinListKind);
        return l.ok ? static_cast<int>(l.count) : -1;
    }

    bool Place(float x, float z, int64_t* outId)
    {
        *outId = 0;
        if (!BytesMatch()) { GS_LOG("[real] not placing a real marker: the game has been patched "
                                    "away from these addresses"); return false; }
        if (g_faultsLeft <= 0) return false;
        if (!gs::pinmodel::Submodule())
        {
            GS_LOG("[real] not placing a real marker: the player's marker copy has not been found "
                   "yet");
            return false;
        }

        // An id of the mod's own. The game hands out a small index into a
        // bitmap, so nothing it does will ever collide with these, and a
        // removal request carrying one is unambiguous.
        const int64_t id = g_nextId.fetch_add(1);
        if (!Mirror(id, x, z))
        {
            --g_faultsLeft;
            GS_LOG_ERR("[real] the client copy would not take id %lld, %d attempt(s) left",
                       static_cast<long long>(id), g_faultsLeft);
            return false;
        }
        g_faultsLeft = 3;
        *outId = id;
        return true;
    }

    int64_t IdBase() { return gs::sig::kModIdBase; }

    int MineInList()
    {
        const gs::pinmodel::List l = ReadList(gs::sig::kPinListKind);
        if (!l.ok) return -1;
        int mine = 0;
        __try
        {
            for (uint32_t i = 0; i < l.count; ++i)
            {
                int64_t id = 0;
                memcpy(&id, reinterpret_cast<const uint8_t*>(l.data) +
                                static_cast<size_t>(i) * gs::sig::kPinRecord, 8);
                if (id >= gs::sig::kModIdBase) ++mine;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        return mine;
    }

    int TakeRetired(int64_t* out, int n)
    {
        int got = 0;
        for (auto& slot : g_retire)
        {
            if (got >= n) break;
            const int64_t id = slot.exchange(0);
            if (id) out[got++] = id;
        }
        return got;
    }

    bool Retire(int64_t id)
    {
        const uintptr_t client = gs::pinmodel::Submodule();
        if (!client || !g_base) return false;
        __try
        {
            const auto* code = reinterpret_cast<const uint8_t*>(g_base + gs::sig::kPinRemove);
            if (!gs::rtti::Readable(code, sizeof(gs::sig::kPinRemovePrologue2))) return false;
            if (memcmp(code, gs::sig::kPinRemovePrologue2,
                       sizeof(gs::sig::kPinRemovePrologue2)) != 0)
            {
                GS_LOG_ERR("[real] the client copy's eraser is not where this build expects it");
                return false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        int32_t err = -1;
        int64_t want = id;
        const auto fn = reinterpret_cast<EraseFn>(g_base + gs::sig::kPinRemove);
        __try
        {
            fn(reinterpret_cast<void*>(client), &err, &want, 0);
        }
        __except (FaultFilter(GetExceptionInformation()))
        {
            const uintptr_t rva = (g_faultAt > g_base) ? g_faultAt - g_base : 0;
            GS_LOG_ERR("[real] erasing id %lld raised 0x%08lX at +0x%08llX",
                       static_cast<long long>(id), g_faultCode,
                       static_cast<unsigned long long>(rva));
            return false;
        }
        const gs::pinmodel::List l = gs::pinmodel::ReadAt(client, gs::sig::kPinListKind);
        // A status of anything but zero is the game's not-found code, which is
        // the ordinary answer when a world has just been rebuilt and the
        // records went with it. It reads as a huge number because the code is
        // registered at runtime, so it is said in words instead.
        if (err == 0)
            GS_LOG_OK("[real] id %lld erased, %u record(s) left",
                      static_cast<long long>(id), l.ok ? l.count : 0u);
        else
            GS_LOG("[real] id %lld was already gone, %u record(s) left",
                   static_cast<long long>(id), l.ok ? l.count : 0u);
        return true;
    }

    void LogState(const char* why)
    {
        bool online = false;
        if (OnlineFlag(&online))
            GS_LOG("[real] %s: the game keeps %s markers here", why, online ? "500" : "15");
        for (int k = 0; k <= 1; ++k)
        {
            const gs::pinmodel::List l = ReadList(k);
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
