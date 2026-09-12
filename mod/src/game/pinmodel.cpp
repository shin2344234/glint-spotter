#include "game/pinmodel.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/player.h"
#include "game/rtti.h"
#include "game/signatures.h"

namespace
{
    // The forty icon kinds, from the enum registration at RVA 0x15262A0. The
    // list is here so the log names what it prints rather than numbering it.
    const char* const kKindNames[] = {
        "Knowledge", "KnowledgeGauge", "UnKnownKnowledge", "Quest", "QuestDiscovered",
        "Item", "Toast", "Mission", "Challenge", "ChallengeQuestGroup",
        "Wanted", "Friendly", "Artifact", "SkillLevelUp", "ExpandInventory",
        "ExpandMercenary", "HireMercenary", "ChangeMercenary", "AutoSave", "LevelUp",
        "FactionRelation", "FactionBlockade", "FactionOperation", "ConnectFactionNodeBuff",
        "FactionResearch", "BountyHunter", "SubLevel", "CallMercenaryCoolTimeEnd",
        "CallHyosiCoolTimeEnd", "RegionChange", "ChallengeComplete", "RandomBox",
        "MissionGauge", "DiscoverInspect", "SharpnessResult", "PopSocket",
        "Debug", "GetLostDropItem", "SequencerTimerGauge", "FactionOperation_Start",
    };
    constexpr int kKindCount = static_cast<int>(sizeof(kKindNames) / sizeof(kKindNames[0]));
}

namespace gs::pinmodel
{
    uintptr_t Submodule()
    {
        const uintptr_t actor = gs::player::Actor();
        if (!actor) return 0;
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(actor + gs::sig::kOff_Actor_Components), 8))
                return 0;
            const uintptr_t comps =
                *reinterpret_cast<const uintptr_t*>(actor + gs::sig::kOff_Actor_Components);
            if (comps < 0x10000) return 0;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(comps + gs::sig::kOff_Comp_PinSubmodule), 8))
                return 0;
            const uintptr_t sub =
                *reinterpret_cast<const uintptr_t*>(comps + gs::sig::kOff_Comp_PinSubmodule);
            if (sub < 0x10000 || (sub & 7) != 0) return 0;
            // The headers run to 0xC8 + forty kinds of sixteen bytes.
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(sub), 0xC8 + 40 * 16)) return 0;
            return sub;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    List Read(int kind)
    {
        List out;
        const uintptr_t sub = Submodule();
        if (!sub || kind < 0 || kind >= 64) return out;
        __try
        {
            out.header = sub + gs::sig::kOff_Pin_Lists + static_cast<uintptr_t>(kind) * 16;
            out.data = *reinterpret_cast<const uintptr_t*>(out.header);
            out.count = *reinterpret_cast<const uint32_t*>(out.header + 8);
            // An empty list is a valid answer and says nothing is wrong; a
            // count with no storage behind it, or storage that cannot be read
            // for the whole run of records, is a wrong offset.
            if (out.count == 0) { out.ok = out.data == 0 || (out.data & 7) == 0; return out; }
            if (out.data < 0x10000 || (out.data & 7) != 0) return out;
            if (out.count > 4096) return out;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(out.data),
                                    static_cast<size_t>(out.count) * gs::sig::kPinRecord))
                return out;
            out.ok = true;
            return out;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out.ok = false;
            return out;
        }
    }

    void LogState(const char* why)
    {
        const uintptr_t sub = Submodule();
        if (!sub)
        {
            GS_LOG("[pins] %s: no submodule at *(*(actor + 0x%llX) + 0x%llX) yet", why,
                   static_cast<unsigned long long>(gs::sig::kOff_Actor_Components),
                   static_cast<unsigned long long>(gs::sig::kOff_Comp_PinSubmodule));
            return;
        }
        const char* cls = nullptr;
        __try
        {
            cls = gs::rtti::VtableClassName(*reinterpret_cast<const void* const*>(sub));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        GS_LOG("[pins] %s: submodule 0x%p%s%s", why, reinterpret_cast<void*>(sub),
               cls ? ", class " : "", cls ? cls : "");

        int nonEmpty = 0;
        for (int k = 0; k < kKindCount; ++k)
        {
            const List l = Read(k);
            if (!l.ok || l.count == 0) continue;
            ++nonEmpty;
            GS_LOG("[pins]   kind %2d %-22s %u record(s) at 0x%p", k, kKindNames[k],
                   l.count, reinterpret_cast<void*>(l.data));
        }
        if (!nonEmpty) GS_LOG("[pins]   every kind's list is empty");

        const List pins = Read(gs::sig::kPinKind);
        if (!pins.ok)
        {
            GS_LOG("[pins] kind 0x15 does not read as a list; header 0x%p holds 0x%p and %u",
                   reinterpret_cast<void*>(pins.header), reinterpret_cast<void*>(pins.data),
                   pins.count);
            return;
        }
        GS_LOG("[pins] kind 0x15, the player's own markers: %u", pins.count);
        __try
        {
            for (uint32_t i = 0; i < pins.count && i < 40; ++i)
            {
                const auto* r = reinterpret_cast<const uint8_t*>(pins.data) +
                                static_cast<size_t>(i) * gs::sig::kPinRecord;
                int64_t id; float x, y, z;
                memcpy(&id, r, 8);
                memcpy(&x, r + 8, 4); memcpy(&y, r + 12, 4); memcpy(&z, r + 16, 4);
                GS_LOG("[pins]   [%u] id %lld at (%.1f, %.1f, %.1f) flags %02X %02X",
                       i, static_cast<long long>(id), x, y, z, r[20], r[21]);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GS_LOG_ERR("[pins] a record faulted while printing");
        }
    }
}
