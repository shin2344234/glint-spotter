#include "game/mapicon.h"

#include <Windows.h>
#include <atomic>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "game/rtti.h"
#include "game/signatures.h"
#include "hook/vtable.h"

namespace
{
    using CreateFn = void* (*)(void*, void*, void*, void*, void*, void*, void*,
                               void*, void*, void*, void*, void*, void*, void*);

    gs::vtable::Swap g_swap[2];
    CreateFn g_orig[2] = {nullptr, nullptr};

    std::atomic<uint64_t> g_seen[2];
    std::mutex g_lastMutex;
    gs::mapicon::Capture g_last[2];

    // How many calls per surface get a full dump. The map creates dozens of icons
    // on open, and the first few say everything the replay needs.
    constexpr uint64_t kFullDumps = 12;
    constexpr uint64_t kSummaryEvery = 50;

    void CopyString(char* dst, size_t cap, const char* src)
    {
        size_t i = 0;
        for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
        dst[i] = 0;
    }

    // Every read of an argument, in one leaf with no C++ objects, inside __try.
    // These pointers are the game's own and valid for the duration of the call,
    // but the arguments after 13 are ours to guess at, and a guess is what a
    // handler is for.
    bool Snapshot(void* const* a, gs::mapicon::Capture* c)
    {
        __try
        {
            c->self = a[0];
            for (int i = 0; i < 14; ++i) c->raw[i] = a[i];

            if (a[1]) c->type = *static_cast<const uint16_t*>(a[1]);
            if (a[2])
            {
                c->keyId = *static_cast<const int64_t*>(a[2]);
                c->keyKind = static_cast<const uint8_t*>(a[2])[8];
            }
            if (a[3]) c->dword4 = *static_cast<const uint32_t*>(a[3]);
            if (a[4]) c->float5 = *static_cast<const float*>(a[4]);
            if (a[5]) memcpy(c->pos, a[5], sizeof(c->pos));

            c->str7Null = a[6] == nullptr;
            if (a[6]) CopyString(c->str7, sizeof(c->str7), static_cast<const char*>(a[6]));
            if (a[7]) CopyString(c->name8, sizeof(c->name8), static_cast<const char*>(a[7]));

            // Bytes by value ride in a full slot with whatever was above them.
            c->byte9 = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(a[8]) & 0xFF);
            if (a[9]) memcpy(c->struct10, a[9], sizeof(c->struct10));
            c->byte11 = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(a[10]) & 0xFF);
            c->ok = true;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            c->ok = false;
            return false;
        }
    }

    void Record(int surface, void* const* a)
    {
        const uint64_t n = ++g_seen[surface];

        gs::mapicon::Capture c{};
        c.surface = surface;
        c.sequence = n;
        Snapshot(a, &c);

        {
            std::lock_guard<std::mutex> lock(g_lastMutex);
            g_last[surface] = c;
        }

        const char* label = surface == 0 ? "world" : "mini";
        if (n <= kFullDumps)
        {
            GS_LOG("[spy %s #%llu] this=0x%p type=0x%04X key=%lld/0x%02X dword4=%u name=\"%s\"%s",
                   label, static_cast<unsigned long long>(n), c.self, c.type,
                   static_cast<long long>(c.keyId), c.keyKind, c.dword4, c.name8,
                   c.ok ? "" : "  (READ FAULTED)");
            GS_LOG("    pos=(%.3f, %.3f, %.3f) float5=%.4f str7=%s byte9=%u byte11=%u",
                   c.pos[0], c.pos[1], c.pos[2], c.float5,
                   c.str7Null ? "null" : c.str7, c.byte9, c.byte11);
            GS_LOG("    struct10: count=%u ptr=0x%016llX  +00 %08X %08X %08X %08X",
                   *reinterpret_cast<const uint32_t*>(c.struct10 + 4),
                   static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(c.struct10 + 0x10)),
                   *reinterpret_cast<const uint32_t*>(c.struct10 + 0),
                   *reinterpret_cast<const uint32_t*>(c.struct10 + 4),
                   *reinterpret_cast<const uint32_t*>(c.struct10 + 8),
                   *reinterpret_cast<const uint32_t*>(c.struct10 + 12));
            GS_LOG("    raw: %p %p %p %p | %p %p %p %p %p %p %p",
                   c.raw[0], c.raw[1], c.raw[2], c.raw[3], c.raw[4], c.raw[5], c.raw[6],
                   c.raw[7], c.raw[8], c.raw[9], c.raw[10]);
        }
        else if (n % kSummaryEvery == 0)
        {
            GS_LOG("[spy %s] %llu calls so far, last type=0x%04X name=\"%s\"",
                   label, static_cast<unsigned long long>(n), c.type, c.name8);
        }
    }

    void* DetourWorld(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                      void* a8, void* a9, void* a10, void* a11, void* a12, void* a13, void* a14)
    {
        void* a[14] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14};
        Record(0, a);
        return g_orig[0](a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }

    void* DetourMini(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                     void* a8, void* a9, void* a10, void* a11, void* a12, void* a13, void* a14)
    {
        void* a[14] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14};
        Record(1, a);
        return g_orig[1](a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

namespace gs::mapicon
{
    int InstallSpy(uintptr_t worldVtable, uintptr_t miniVtable)
    {
        int installed = 0;
        const uintptr_t vt[2] = {worldVtable, miniVtable};
        void* det[2] = {reinterpret_cast<void*>(&DetourWorld), reinterpret_cast<void*>(&DetourMini)};
        const char* label[2] = {"world map", "minimap"};

        for (int i = 0; i < 2; ++i)
        {
            if (!vt[i]) continue;
            if (!gs::vtable::Install(vt[i], gs::sig::kSlotCreateIcon, det[i], g_swap[i]))
            {
                GS_LOG_ERR("spy: could not take slot %d on the %s vtable", gs::sig::kSlotCreateIcon, label[i]);
                continue;
            }
            g_orig[i] = reinterpret_cast<CreateFn>(g_swap[i].original);
            GS_LOG_OK("spy: slot %d on %s vtable 0x%p was 0x%p, now ours; forwarding to the original",
                      gs::sig::kSlotCreateIcon, label[i], reinterpret_cast<void*>(vt[i]), g_swap[i].original);
            ++installed;
        }
        return installed;
    }

    void RemoveSpy()
    {
        for (int i = 0; i < 2; ++i)
        {
            if (!g_swap[i].installed) continue;
            bool leftAlone = false;
            if (gs::vtable::Restore(g_swap[i], leftAlone))
                GS_LOG("spy: slot restored on surface %d", i);
            else if (leftAlone)
                GS_LOG("spy: surface %d slot now holds someone else's hook, left in place", i);
        }
    }

    uint64_t Seen(int surface) { return g_seen[surface].load(); }

    bool Last(int surface, Capture& out)
    {
        std::lock_guard<std::mutex> lock(g_lastMutex);
        if (g_last[surface].sequence == 0) return false;
        out = g_last[surface];
        return true;
    }
}
