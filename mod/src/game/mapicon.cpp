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
                c->key3q = *static_cast<const uint64_t*>(a[2]);
                c->key3b = static_cast<const uint8_t*>(a[2])[8];
            }
            if (a[3]) c->dword4 = *static_cast<const uint32_t*>(a[3]);
            if (a[4])
            {
                c->key5q = *static_cast<const uint64_t*>(a[4]);
                c->key5b = static_cast<const uint8_t*>(a[4])[8];
            }
            if (a[5]) c->dword6 = *static_cast<const uint32_t*>(a[5]);
            if (a[6]) c->float7 = *static_cast<const float*>(a[6]);
            if (a[7]) memcpy(c->pos, a[7], sizeof(c->pos));

            c->str9Null = a[8] == nullptr;
            if (a[8]) CopyString(c->str9, sizeof(c->str9), static_cast<const char*>(a[8]));
            if (a[9]) CopyString(c->name10, sizeof(c->name10), static_cast<const char*>(a[9]));

            c->byte11 = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(a[10]) & 0xFF);
            if (a[11]) memcpy(c->struct12, a[11], sizeof(c->struct12));
            c->byte13 = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(a[12]) & 0xFF);
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
            GS_LOG("[spy %s #%llu] this=0x%p type=0x%04X name=\"%s\" pos=(%.3f, %.3f, %.3f)%s",
                   label, static_cast<unsigned long long>(n), c.self, c.type, c.name10,
                   c.pos[0], c.pos[1], c.pos[2], c.ok ? "" : "  (READ FAULTED)");
            GS_LOG("    key3=%016llX/%02X dword4=%u key5=%016llX/%02X dword6=%u float7=%.4f",
                   static_cast<unsigned long long>(c.key3q), c.key3b, c.dword4,
                   static_cast<unsigned long long>(c.key5q), c.key5b, c.dword6, c.float7);
            GS_LOG("    str9=%s byte11=%u byte13=%u struct12.count=%u struct12.ptr=0x%016llX",
                   c.str9Null ? "null" : c.str9, c.byte11, c.byte13,
                   *reinterpret_cast<const uint32_t*>(c.struct12 + 4),
                   static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(c.struct12 + 0x10)));
            GS_LOG("    raw: %p %p %p %p | %p %p %p %p %p | %p %p %p %p %p",
                   c.raw[0], c.raw[1], c.raw[2], c.raw[3], c.raw[4], c.raw[5], c.raw[6],
                   c.raw[7], c.raw[8], c.raw[9], c.raw[10], c.raw[11], c.raw[12], c.raw[13]);
        }
        else if (n % kSummaryEvery == 0)
        {
            GS_LOG("[spy %s] %llu calls so far, last type=0x%04X name=\"%s\"",
                   label, static_cast<unsigned long long>(n), c.type, c.name10);
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
