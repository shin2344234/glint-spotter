#include "game/actors.h"

#include <Windows.h>
#include <atomic>

#include "core/log.h"
#include "game/rtti.h"

namespace
{
    // Master Looter's offsets, signatures.h in that project, build 2.01.00.
    constexpr uintptr_t kOff_Mgr_List = 0x190;   // {u32 count, u32 cap, ptr} of entity pointers
    constexpr uintptr_t kOff_Ent_Eid  = 0x60;    // u32 actor id
    constexpr uint32_t  kMaxWalk      = 65536;   // a sane ceiling on the list

    std::atomic<void*> g_mgr{nullptr};
    std::atomic<uint32_t> g_lastCount{0};
    int g_describeLeft = 2;

    // The whole walk in a leaf with no C++ objects, inside a handler: the list
    // is the game's and it changes under us on other threads.
    uintptr_t Walk(uintptr_t mgr, uint32_t eid, uint32_t* countOut, bool describe)
    {
        __try
        {
            const uintptr_t list = mgr + kOff_Mgr_List;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(list), 16)) return 0;
            const uint32_t count = *reinterpret_cast<const uint32_t*>(list);
            const uint32_t cap = *reinterpret_cast<const uint32_t*>(list + 4);
            const uintptr_t data = *reinterpret_cast<const uintptr_t*>(list + 8);
            *countOut = count;
            if (describe) GS_LOG("[actors] manager 0x%p list: count %u cap %u data 0x%p",
                                 reinterpret_cast<void*>(mgr), count, cap, reinterpret_cast<void*>(data));
            if (!data || count == 0 || count > kMaxWalk || cap < count) return 0;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(data), static_cast<size_t>(count) * sizeof(uintptr_t)))
                return 0;

            const auto* ents = reinterpret_cast<const uintptr_t*>(data);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uintptr_t e = ents[i];
                if (!e || !gs::rtti::Readable(reinterpret_cast<const void*>(e + kOff_Ent_Eid), 4)) continue;
                if (*reinterpret_cast<const uint32_t*>(e + kOff_Ent_Eid) == eid) return e;
            }
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }
}

namespace gs::actors
{
    void SetManager(void* manager) { g_mgr.store(manager); }
    bool Ready() { return g_mgr.load() != nullptr; }

    uintptr_t ByEid(uint32_t eid)
    {
        const auto mgr = reinterpret_cast<uintptr_t>(g_mgr.load());
        if (!mgr || eid == 0 || eid == 0xFFFFFFFF) return 0;
        const bool describe = g_describeLeft > 0;
        if (describe) --g_describeLeft;
        uint32_t count = 0;
        const uintptr_t e = Walk(mgr, eid, &count, describe);
        g_lastCount.store(count);
        return e;
    }

    uint32_t LastCount() { return g_lastCount.load(); }
}
