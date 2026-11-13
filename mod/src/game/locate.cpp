#include "game/locate.h"

#include <Windows.h>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "game/rtti.h"
#include "game/signatures.h"
#include "game/typescan.h"

namespace
{
    constexpr const char* kLgsoClass  = ".?AVLevelGimmickSceneObjectInfoManager@pa@@";
    constexpr const char* kModeClass  = ".?AVSpecialModeInfoManager@pa@@";
    constexpr const char* kCreateReq  = ".?AVTrocTrCreateOrChangePinMarkerReq@pa@@";
    constexpr const char* kRemoveReq  = ".?AVTrocTrRemovePinMarkerReq@pa@@";
    constexpr const char* kCreateAck  = ".?AVTrocTrCreateOrChangePinMarkerAck@pa@@";
    constexpr const char* kRemoveAck  = ".?AVTrocTrRemovePinMarkerAck@pa@@";

    // Slot 2 of each message class is its handler, and of each manager its
    // setter. The longest handler on 3663 is 0x746 bytes.
    constexpr int kSlot = 2;
    constexpr size_t kHandlerSpan = 0x800;
    constexpr size_t kCreateSpan = 0x200;

    enum How { kNone, kRecorded, kFound, kRecordedUnproved };

    struct Entry
    {
        const char* name;
        uintptr_t recorded;
        uintptr_t rva = 0;
        How how = kNone;
        const char* route = "";
    };

    Entry g_lgso{"kLgsoManagerGlobal", gs::sig::kLgsoManagerGlobal};
    Entry g_mode{"kSpecialModeManagerGlobal", gs::sig::kSpecialModeManagerGlobal};
    Entry g_create{"kPinCreate", gs::sig::kPinCreate};
    Entry g_remove{"kPinServerRemove", gs::sig::kPinServerRemove};
    Entry g_upsert{"kPinUpsert", gs::sig::kPinUpsert};
    Entry g_erase{"kPinRemove", gs::sig::kPinRemove};
    Entry g_flag{"kPinOnlineFlag", gs::sig::kPinOnlineFlag};

    std::once_flag g_once;
    uintptr_t g_base = 0;
    size_t g_size = 0;

    bool InImage(uintptr_t va, size_t bytes)
    {
        return va >= g_base && va + bytes <= g_base + g_size;
    }

    bool Matches(uintptr_t va, const uint8_t* bytes, size_t n)
    {
        if (!InImage(va, n)) return false;
        __try
        {
            return memcmp(reinterpret_cast<const void*>(va), bytes, n) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uintptr_t SlotFn(uintptr_t vt)
    {
        if (!vt) return 0;
        __try
        {
            const uintptr_t fn = *reinterpret_cast<const uintptr_t*>(vt + kSlot * 8);
            return InImage(fn, 16) ? fn : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // mov qword ptr [rip + d], rcx ; ret
    uintptr_t SetterTarget(uintptr_t fn)
    {
        if (!fn) return 0;
        __try
        {
            const auto* p = reinterpret_cast<const uint8_t*>(fn);
            if (p[0] != 0x48 || p[1] != 0x89 || p[2] != 0x0D || p[7] != 0xC3) return 0;
            int32_t d;
            memcpy(&d, p + 3, 4);
            const uintptr_t at = fn + 7 + static_cast<intptr_t>(d);
            return InImage(at, 8) ? at - g_base : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // The one distinct target of an E8 call in the handler's bytes that starts
    // with the prologue. Read as bytes rather than decoded, so an E8 inside
    // another instruction can turn up as a candidate; the prologue throws
    // those out, and two distinct matches count as none.
    uintptr_t CallMatching(uintptr_t fn, const uint8_t* pro, size_t n, int* seen)
    {
        *seen = 0;
        if (!fn || !InImage(fn, kHandlerSpan)) return 0;
        uintptr_t hit = 0;
        __try
        {
            const auto* p = reinterpret_cast<const uint8_t*>(fn);
            for (size_t i = 0; i + 5 <= kHandlerSpan; ++i)
            {
                if (p[i] != 0xE8) continue;
                int32_t rel;
                memcpy(&rel, p + i + 1, 4);
                const uintptr_t t = fn + i + 5 + static_cast<intptr_t>(rel);
                if (!InImage(t, n) || !Matches(t, pro, n)) continue;
                if (t == hit) continue;
                ++*seen;
                hit = t;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return *seen == 1 ? hit - g_base : 0;
    }

    // cmp byte ptr [rip + d], imm8, 80 3D d32 ib
    uintptr_t FlagIn(uintptr_t fn, int* seen)
    {
        *seen = 0;
        if (!fn || !InImage(fn, kCreateSpan)) return 0;
        uintptr_t hit = 0;
        __try
        {
            const auto* p = reinterpret_cast<const uint8_t*>(fn);
            for (size_t i = 0; i + 7 <= kCreateSpan; ++i)
            {
                if (p[i] != 0x80 || p[i + 1] != 0x3D) continue;
                int32_t d;
                memcpy(&d, p + i + 2, 4);
                const uintptr_t t = fn + i + 7 + static_cast<intptr_t>(d);
                if (!InImage(t, 1) || t == hit) continue;
                ++*seen;
                hit = t;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return *seen == 1 ? hit - g_base : 0;
    }

    void Function(Entry& e, uintptr_t vt, const char* cls, const uint8_t* pro, size_t n)
    {
        if (Matches(g_base + e.recorded, pro, n))
        {
            e.rva = e.recorded;
            e.how = kRecorded;
            return;
        }
        int seen = 0;
        e.rva = CallMatching(SlotFn(vt), pro, n, &seen);
        e.how = e.rva ? kFound : kNone;
        e.route = cls;
    }

    void Global(Entry& e, uintptr_t vt, const char* cls)
    {
        e.route = cls;
        e.rva = SetterTarget(SlotFn(vt));
        if (e.rva)
        {
            e.how = e.rva == e.recorded ? kRecorded : kFound;
            return;
        }
        // RTTI did not give it. The recorded value is all there is, and the
        // readers check the object behind it before they trust it.
        e.rva = e.recorded;
        e.how = kRecordedUnproved;
    }

    void Run()
    {
        if (!gs::typescan::ModuleRange(g_base, g_size)) return;
        const char* const kw[] = {kLgsoClass, kModeClass, kCreateReq, kRemoveReq, kCreateAck, kRemoveAck};
        static gs::typescan::ClassInfo found[16];
        const size_t n = gs::typescan::FindClasses(kw, 6, found, 16);
        auto vt = [&](const char* cls) -> uintptr_t
        {
            for (size_t i = 0; i < n; ++i)
                if (found[i].vtableVa && strcmp(found[i].name, cls) == 0) return found[i].vtableVa;
            return 0;
        };

        Global(g_lgso, vt(kLgsoClass), kLgsoClass);
        Global(g_mode, vt(kModeClass), kModeClass);
        Function(g_create, vt(kCreateReq), kCreateReq, gs::sig::kPinCreatePrologue, sizeof(gs::sig::kPinCreatePrologue));
        Function(g_remove, vt(kRemoveReq), kRemoveReq, gs::sig::kPinRemovePrologue, sizeof(gs::sig::kPinRemovePrologue));
        Function(g_upsert, vt(kCreateAck), kCreateAck, gs::sig::kPinUpsertPrologue, sizeof(gs::sig::kPinUpsertPrologue));
        Function(g_erase, vt(kRemoveAck), kRemoveAck, gs::sig::kPinRemovePrologue2, sizeof(gs::sig::kPinRemovePrologue2));

        // The flag is read out of whichever create was settled on, so it
        // follows the create rather than the record.
        g_flag.route = "the create's cmp byte ptr [rip+...]";
        int seen = 0;
        g_flag.rva = g_create.rva ? FlagIn(g_base + g_create.rva, &seen) : 0;
        if (g_flag.rva)
            g_flag.how = g_flag.rva == g_flag.recorded ? kRecorded : kFound;
        else
            g_flag.how = kNone;
    }

    uintptr_t Get(const Entry& e)
    {
        gs::locate::Resolve();
        return e.rva;
    }
}

namespace gs::locate
{
    void Resolve()
    {
        std::call_once(g_once, Run);
    }

    uintptr_t LgsoGlobal()        { return Get(g_lgso); }
    uintptr_t SpecialModeGlobal() { return Get(g_mode); }
    uintptr_t PinCreate()         { return Get(g_create); }
    uintptr_t PinServerRemove()   { return Get(g_remove); }
    uintptr_t PinUpsert()         { return Get(g_upsert); }
    uintptr_t PinRemove()         { return Get(g_erase); }
    uintptr_t PinOnlineFlag()     { return Get(g_flag); }

    void Report(int& ok, int& stale, int& missing, const char** names, int cap)
    {
        Resolve();
        const Entry* all[] = {&g_lgso, &g_mode, &g_create, &g_remove, &g_upsert, &g_erase, &g_flag};
        for (const Entry* e : all)
        {
            const char* route = e->route;
            if (route[0] == '.' && route[1] == '?') route += 4;   // drop .?AV for the log
            switch (e->how)
            {
            case kRecorded:
                ++ok;
                GS_LOG_OK("signatures.h %s +0x%08llX still matches", e->name,
                          static_cast<unsigned long long>(e->recorded));
                break;
            case kFound:
                ++stale;
                GS_LOG_OK("signatures.h %s +0x%08llX is stale; found at +0x%08llX through %s", e->name,
                          static_cast<unsigned long long>(e->recorded),
                          static_cast<unsigned long long>(e->rva), route);
                break;
            case kRecordedUnproved:
                ++stale;
                GS_LOG("signatures.h %s: RTTI did not offer %s, so the recorded +0x%08llX is used unchecked",
                       e->name, route, static_cast<unsigned long long>(e->recorded));
                break;
            default:
                if (missing < cap) names[missing] = e->name;
                ++missing;
                GS_LOG_ERR("signatures.h %s +0x%08llX no longer matches and %s did not give it again",
                           e->name, static_cast<unsigned long long>(e->recorded), route);
                break;
            }
        }
    }
}
