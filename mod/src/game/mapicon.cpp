#include "game/mapicon.h"

#include <Windows.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "core/settings.h"
#include "game/rtti.h"
#include "game/signatures.h"
#include "hook/pad.h"
#include "hook/vtable.h"

namespace
{
    using CreateFn = void* (*)(void*, void*, void*, void*, void*, void*, void*,
                               void*, void*, void*, void*, void*, void*, void*);

    gs::vtable::Swap g_swap[2];
    CreateFn g_orig[2] = {nullptr, nullptr};
    gs::vtable::Swap g_rmSwap[2];
    CreateFn g_rmOrig[2] = {nullptr, nullptr};
    std::atomic<uint64_t> g_rmSeen[2];
    int g_rmLogsLeft = 40;

    std::atomic<uint64_t> g_seen[2];
    std::mutex g_lastMutex;
    gs::mapicon::Capture g_last[2];
    void CopyString(char* dst, size_t cap, const char* src);
    gs::mapicon::Capture g_lastPin;
    gs::mapicon::Capture g_lastPlayer;
    std::atomic<bool> g_replayPending{false};
    std::atomic<uint64_t> g_replayCount{0};

    // Every pin this mod placed this session, for the one-per-area rule.
    constexpr int kMaxPins = 256;
    struct Placed { float x, y, z; char label[16]; bool drawn; bool gone; int64_t id; };
    Placed g_placed[kMaxPins];
    std::atomic<int> g_placedN{0};
    std::atomic<bool> g_repinWanted{false};


    // Only a control the running game still says is a world map root gets
    // called. The stored pointer comes from the spy and the UI is free to
    // destroy and rebuild that object; a call into freed memory returns
    // whatever it likes and draws nothing, which is one of the two ways a
    // buzz can arrive with no marker behind it.
    bool RootLooksRight(void* root)
    {
        if (!root) return false;
        if (!gs::rtti::Readable(root, 8)) return false;
        const void* vt = *reinterpret_cast<void* const*>(root);
        return gs::rtti::VtableIs(vt, gs::sig::kWorldMapClass);
    }

    bool IsName(const gs::mapicon::Capture& c, const char* name)
    {
        return strcmp(c.name8, name) == 0;
    }

    // Every distinct icon name seen this session, so the first sight of a new
    // one is logged in full no matter how many icons came before it. This is
    // how a session with the flash tells us whether glints create icons.
    constexpr size_t kMaxNames = 96;
    char g_names[kMaxNames][64];
    size_t g_nameCount = 0;
    std::atomic<void*> g_lastWorldRoot{nullptr};
    std::atomic<void*> g_lastMiniRoot{nullptr};
    int g_pinCallerLeft = 12;

    bool NewName(const char* name)
    {
        for (size_t i = 0; i < g_nameCount; ++i)
            if (strcmp(g_names[i], name) == 0) return false;
        if (g_nameCount < kMaxNames)
        {
            CopyString(g_names[g_nameCount], sizeof(g_names[0]), name);
            ++g_nameCount;
        }
        return true;
    }

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
            if (a[9])
            {
                memcpy(c->struct10, a[9], sizeof(c->struct10));
                // Five pins with different shapes and colours were identical in
                // every argument, so the style is behind this pointer or set later.
                const uint8_t* pt = *reinterpret_cast<uint8_t* const*>(c->struct10 + 0x10);
                if (pt && gs::rtti::Readable(pt, sizeof(c->pointee)))
                {
                    memcpy(c->pointee, pt, sizeof(c->pointee));
                    c->pointeeOk = true;
                }
            }
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

    void Record(int surface, void* const* a, uintptr_t ret)
    {
        const uint64_t n = ++g_seen[surface];

        gs::mapicon::Capture c{};
        c.surface = surface;
        c.sequence = n;
        Snapshot(a, &c);

        const bool pin = c.ok && IsName(c, "MapIcon_Pin_Marker");
        const bool player = c.ok && IsName(c, "MapIcon_ActorFocus");
        bool fresh = false;
        {
            std::lock_guard<std::mutex> lock(g_lastMutex);
            g_last[surface] = c;
            if (pin && surface == 0) g_lastPin = c;
            if (player && surface == 0) g_lastPlayer = c;
            if (c.ok) fresh = NewName(c.name8);
        }
        // The one thing worth knowing that a captured argument cannot say:
        // who called. When the game builds one of its own markers, the return
        // address names the function that holds the marker list, and that list
        // is what the mod has to write into to be able to delete a
        // pin. Session eighty-one hunted for the list by the coordinates it
        // must contain and found a position trail instead, because I happen
        // to stand near one of my own markers. A caller cannot be
        // coincidence.
        // Who called, and out of which module.
        //
        // The address is passed in from the detour now. Session ninety-two
        // took it here instead and the answer was GlintSpotter.asi+0x6D56,
        // which is this function's own caller: the detour. One frame too low,
        // and a reminder that _ReturnAddress only ever names the frame it is
        // written in.
        //
        // The frame that matters is the game's, and the detour is standing in
        // it. Whatever module that turns out to be is where a marker is
        // created, and therefore where deleting one has to happen.
        if (pin && g_pinCallerLeft > 0 && ret)
        {
            --g_pinCallerLeft;
            HMODULE owner = nullptr;
            wchar_t path[MAX_PATH]{};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(ret), &owner) && owner)
            {
                GetModuleFileNameW(owner, path, MAX_PATH);
                const wchar_t* leaf = wcsrchr(path, L'\\');
                GS_LOG("[spy] a MapIcon_Pin_Marker create came from %ls+0x%llX (surface %d, key %lld)",
                       leaf ? leaf + 1 : path,
                       static_cast<unsigned long long>(ret - reinterpret_cast<uintptr_t>(owner)),
                       surface, static_cast<long long>(c.keyId));

                // And the frames above it, because one return address only
                // names the function that made the call and not the one that
                // decided to. CrimsonDesert.exe+0xD66C86 turned out to be a
                // per icon creator taking a key it was handed; whoever walks
                // the marker list and hands them out is further up, and
                // chasing that a frame per session is a session per frame.
                void* frames[10]{};
                const USHORT got = RtlCaptureStackBackTrace(1, 10, frames, nullptr);
                const uintptr_t exeBase =
                    reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                char line[400];
                int w = 0;
                for (USHORT f = 0; f < got && w + 24 < static_cast<int>(sizeof(line)); ++f)
                {
                    const uintptr_t a = reinterpret_cast<uintptr_t>(frames[f]);
                    const int k = (a > exeBase && a - exeBase < 0x18000000)
                        ? _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " +%llX",
                                      static_cast<unsigned long long>(a - exeBase))
                        : _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " [%p]", frames[f]);
                    if (k < 0) break;
                    w += k;
                }
                line[w] = 0;
                GS_LOG("[spy]   the frames above it:%s", line);
            }
            else
            {
                GS_LOG("[spy] a MapIcon_Pin_Marker create came from 0x%p, which belongs to no "
                       "loaded module (surface %d, key %lld)", reinterpret_cast<void*>(ret),
                       surface, static_cast<long long>(c.keyId));
            }
        }
        // The player's own marker is created when the map opens, and the
        // game rebuilds every icon around it. Anything the mod drew is gone
        // at that moment, so this is the cue to draw it again.
        if (surface == 0 && IsName(c, "MapIcon_ActorFocus")) g_repinWanted.store(true);
        if (surface == 0) g_lastWorldRoot.store(c.self);
        else g_lastMiniRoot.store(c.self);
        if (n == 1) GS_LOG("[spy %s] calls arrive on thread %lu", surface == 0 ? "world" : "mini", GetCurrentThreadId());

        const char* label = surface == 0 ? "world" : "mini";
        // A pin marker is the call the replay copies, and a name never seen
        // before is what a flash session is for, so both are dumped in full no
        // matter how many icons came before them.
        if (fresh) GS_LOG("[spy %s] new icon name: \"%s\"", label, c.name8);
        if (n <= kFullDumps || pin || fresh)
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
            if (pin && c.pointeeOk)
            {
                const uint8_t* q = c.pointee;
                GS_LOG("    pointee +00 %02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X"
                       "  +10 %02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X",
                       q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],q[8],q[9],q[10],q[11],q[12],q[13],q[14],q[15],
                       q[16],q[17],q[18],q[19],q[20],q[21],q[22],q[23],q[24],q[25],q[26],q[27],q[28],q[29],q[30],q[31]);
                GS_LOG("    pointee +20 %02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X"
                       "  +30 %02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X",
                       q[32],q[33],q[34],q[35],q[36],q[37],q[38],q[39],q[40],q[41],q[42],q[43],q[44],q[45],q[46],q[47],
                       q[48],q[49],q[50],q[51],q[52],q[53],q[54],q[55],q[56],q[57],q[58],q[59],q[60],q[61],q[62],q[63]);
            }
        }
        else if (n % kSummaryEvery == 0)
        {
            GS_LOG("[spy %s] %llu calls so far, last type=0x%04X name=\"%s\"",
                   label, static_cast<unsigned long long>(n), c.type, c.name8);
        }
    }

    // One call of our own, made on the thread the game just used for its own.
    // Every pointer argument points at our copies, because the game's were on
    // its stack and are gone. The struct at argument 10 is passed zeroed: the
    // captured one may carry a pointer into memory the game has since freed,
    // a zero count makes the dispatcher skip it, and the constructor swaps it
    // in as the object's own resting state.
    void Replay(void* self)
    {
        gs::mapicon::Capture pin, player;
        {
            std::lock_guard<std::mutex> lock(g_lastMutex);
            pin = g_lastPin;
            player = g_lastPlayer;
        }
        if (pin.sequence == 0)
        {
            GS_LOG_ERR("[replay] no MapIcon_Pin_Marker captured yet. Place a custom marker on the map first.");
            return;
        }

        const uint64_t n = ++g_replayCount;

        uint16_t type = pin.type;
        struct { int64_t id; uint8_t kind; uint8_t pad[7]; } key{1000 + static_cast<int64_t>(n), pin.keyKind, {}};
        uint32_t dword4 = pin.dword4;
        float float5 = pin.float5;
        // Offset from the pin the player just placed. The player marker looked
        // like the better anchor and is not: it is created once at first map
        // open and never refreshed, so session nine had it ten minutes stale.
        // Pins carry no elevation, and 30 units is far enough apart to see.
        (void)player;
        float pos[3] = {pin.pos[0] + 30.0f, 0.0f, pin.pos[2] + 30.0f};
        char str7[48];
        memcpy(str7, pin.str7, sizeof(str7));
        char name8[64];
        memcpy(name8, pin.name8, sizeof(name8));
        uint8_t struct10[36]{};

        GS_LOG("[replay #%llu] calling slot 170 on 0x%p: type=0x%04X key=%lld/0x%02X dword4=%u name=\"%s\"",
               static_cast<unsigned long long>(n), self, type, static_cast<long long>(key.id), key.kind,
               dword4, name8);
        GS_LOG("[replay #%llu]   pos=(%.3f, %.3f, %.3f) float5=%.4f str7=%s byte9=%u byte11=%u struct10=zeroed (captured count was %u)",
               static_cast<unsigned long long>(n), pos[0], pos[1], pos[2], float5,
               pin.str7Null ? "null" : str7, pin.byte9, pin.byte11,
               *reinterpret_cast<const uint32_t*>(pin.struct10 + 4));

        void* result = g_orig[0](self, &type, &key, &dword4, &float5, pos,
                                 pin.str7Null ? nullptr : static_cast<void*>(str7),
                                 name8,
                                 reinterpret_cast<void*>(static_cast<uintptr_t>(pin.byte9)),
                                 struct10,
                                 reinterpret_cast<void*>(static_cast<uintptr_t>(pin.byte11)),
                                 nullptr, nullptr, nullptr);

        GS_LOG_OK("[replay #%llu] returned 0x%p. A second pin 30 units from the one you placed is ours.",
                  static_cast<unsigned long long>(n), result);
    }

    // Slot 171, watched and forwarded unchanged.
    //
    // Its prologue homes a dword, a sixteen byte key and a word, in the same
    // order slot 170 homes its first three, so the reader below prints them
    // that way. Everything is forwarded regardless, so a wrong reading costs a
    // confusing log line and nothing else.
    void RecordRemove(int surface, void* const* a, uintptr_t ret)
    {
        const uint64_t n = ++g_rmSeen[surface];
        if (g_rmLogsLeft <= 0) return;
        __try
        {
            const uint16_t type = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(a[1]));
            const uint32_t d4 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a[3]));
            int64_t id = 0; uint8_t kind = 0;
            bool keyOk = false;
            if (a[2] && gs::rtti::Readable(a[2], 16))
            {
                memcpy(&id, a[2], 8);
                memcpy(&kind, static_cast<const uint8_t*>(a[2]) + 8, 1);
                keyOk = true;
            }
            // Only the player's own pin markers.
            //
            // Session a hundred and one spent the whole budget on the map's
            // housekeeping: opening and closing it removes icons of kind 0x0C
            // and 0x0E by the dozen, and the actual deletion happened after
            // the twenty-fourth of them and was never printed. Kind 0x15 is
            // the one that matters and it is rare, so the budget lasts.
            if (!keyOk || kind != gs::sig::kPinKind) return;

            // The mod's own removals come through here too, since it takes an
            // icon off by calling this very slot. Forgetting the pin is the
            // caller's job there, in the right order and with the position in
            // hand, so nothing is done about it from inside the spy.
            --g_rmLogsLeft;
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            GS_LOG("[rmspy %s #%llu] this=0x%p type=0x%04X key=%lld/0x%02X dword4=%u, called from "
                   "+0x%08llX", surface == 0 ? "world" : "mini",
                   static_cast<unsigned long long>(n), a[0], type,
                   keyOk ? static_cast<long long>(id) : -1, kind, d4,
                   static_cast<unsigned long long>(ret > base ? ret - base : 0));
            GS_LOG("[rmspy]   raw: %p %p %p %p %p %p", a[0], a[1], a[2], a[3], a[4], a[5]);

            void* frames[8]{};
            const USHORT got = RtlCaptureStackBackTrace(1, 8, frames, nullptr);
            char line[300];
            int w = 0;
            for (USHORT f = 0; f < got && w + 20 < static_cast<int>(sizeof(line)); ++f)
            {
                const uintptr_t x = reinterpret_cast<uintptr_t>(frames[f]);
                const int k = (x > base && x - base < 0x18000000)
                    ? _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " +%llX",
                                  static_cast<unsigned long long>(x - base))
                    : _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " [%p]", frames[f]);
                if (k < 0) break;
                w += k;
            }
            line[w] = 0;
            GS_LOG("[rmspy]   frames:%s", line);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GS_LOG_ERR("[rmspy] a read faulted describing a removal; the call itself is untouched");
        }
    }

    void* DetourRemoveWorld(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                            void* a8, void* a9, void* a10, void* a11, void* a12, void* a13, void* a14)
    {
        void* const a[14] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14};
        RecordRemove(0, a, reinterpret_cast<uintptr_t>(_ReturnAddress()));
        return g_rmOrig[0](a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }

    void* DetourRemoveMini(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                           void* a8, void* a9, void* a10, void* a11, void* a12, void* a13, void* a14)
    {
        void* const a[14] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14};
        RecordRemove(1, a, reinterpret_cast<uintptr_t>(_ReturnAddress()));
        return g_rmOrig[1](a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }

    void* DetourWorld(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                      void* a8, void* a9, void* a10, void* a11, void* a12, void* a13, void* a14)
    {
        void* a[14] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14};
        Record(0, a, reinterpret_cast<uintptr_t>(_ReturnAddress()));
        void* r = g_orig[0](a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);

        // The game's call is done and we are on its thread with its controller
        // in hand. Session nine showed the map creates its icons once and then
        // only when a pin is placed, so a pin placement is the moment, and it is
        // also the call the replay copies, captured a few lines up.
        if (g_replayPending.load())
        {
            gs::mapicon::Capture last;
            {
                std::lock_guard<std::mutex> lock(g_lastMutex);
                last = g_last[0];
            }
            if (last.ok && IsName(last, "MapIcon_Pin_Marker") && g_replayPending.exchange(false))
                Replay(a1);
        }
        return r;
    }

    void* DetourMini(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                     void* a8, void* a9, void* a10, void* a11, void* a12, void* a13, void* a14)
    {
        void* a[14] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14};
        Record(1, a, reinterpret_cast<uintptr_t>(_ReturnAddress()));
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

    bool LastPin(Capture& out)
    {
        std::lock_guard<std::mutex> lock(g_lastMutex);
        if (g_lastPin.sequence == 0) return false;
        out = g_lastPin;
        return true;
    }

    bool LastPlayer(Capture& out)
    {
        std::lock_guard<std::mutex> lock(g_lastMutex);
        if (g_lastPlayer.sequence == 0) return false;
        out = g_lastPlayer;
        return true;
    }

    void* PlacePinNow(void* worldRoot, float x, float y, float z, const char* labelText,
                      int64_t keyId, bool haveKey)
    {
        if (!worldRoot || !g_orig[0])
        {
            GS_LOG_ERR("[pin] no world root or no original slot 170, nothing placed");
            return nullptr;
        }
        if (!RootLooksRight(worldRoot))
        {
            GS_LOG_ERR("[pin] 0x%p no longer reads as a world map root; nothing placed and the "
                       "stored pointer is dropped", worldRoot);
            g_lastWorldRoot.store(nullptr);
            return nullptr;
        }
        const uint64_t n = ++g_replayCount;

        // Session ten, byte for byte, except the position and the key id.
        uint16_t type = 0x0001;
        struct { int64_t id; uint8_t kind; uint8_t pad[7]; } key{
            haveKey ? keyId : 1000 + static_cast<int64_t>(n), 0x15, {}};
        uint32_t dword4 = 0;
        float float5 = 0.0f;
        // Height zero, because that is what the game passes for this icon.
        // Session forty-seven caught it creating a Pin_Marker of its own when
        // I placed a marker by hand: name "MapIcon_Pin_Marker", type
        // 0x0001, key 0/0x15, label "Marker", pos (-9714.073, 0.000,
        // -4141.196), with the player standing at (-9710.511, 569.144,
        // -4259.204). The x and z are world coordinates in the player's own
        // frame and the height is zero. Version 0.19.1 started sending the
        // node's real height on the strength of the ActorFocus icon, which is
        // a different icon of a different type, and this one wants zero.
        float pos[3] = {x, 0.0f, z};
        (void)y;
        char label[48];
        CopyString(label, sizeof(label), labelText ? labelText : "Marker");
        char name[64] = "MapIcon_Pin_Marker";
        uint8_t struct10[36]{};

        GS_LOG("[pin #%llu] slot 170 on 0x%p: key=%lld/0x15 pos=(%.3f, %.3f, %.3f) label=\"%s\" thread %lu",
               static_cast<unsigned long long>(n), worldRoot, static_cast<long long>(key.id), x, y, z, label,
               GetCurrentThreadId());
        void* r = g_orig[0](worldRoot, &type, &key, &dword4, &float5, pos, label, name,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(0)), struct10,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(1)),
                            nullptr, nullptr, nullptr);
        GS_LOG_OK("[pin #%llu] returned 0x%p", static_cast<unsigned long long>(n), r);

        // Nothing else happens when the game made no icon. Not the buzz, which
        // would report a pin that is not there; not the minimap copy, which
        // would be left behind when the caller retries; and not the record
        // below, which would leave the mod holding a pin nothing can see.
        if (!r)
        {
            GS_LOG_ERR("[pin #%llu] the game made no icon, so this pin is not written down",
                       static_cast<unsigned long long>(n));
            return nullptr;
        }

        // Said immediately, before anything optional runs. The map is not on
        // screen when a pin lands, so this buzz is the only thing that tells
        // it happened, and it should not be waiting behind a feature that
        // might not survive the frame.
        // 450 ms, up from 220: at 220 a pin landing mid-fight or on a horse
        // was easy to miss.
        if (gs::Settings::Get().rumble) gs::pad::Buzz(28000, 450);

        // The same icon on the minimap, only when the ini asks.
        //
        // Its own key id, a hundred thousand above the world map's. The two
        // calls shared one id in 0.36.0, and if the game keys icons by id
        // alone then the second call was not adding a copy but moving the
        // first one onto the other surface, which would take the pin off the
        // map I was looking at. That is a guess about why a working
        // feature stopped working, and a guess is reason enough to give the
        // copy its own id and leave it switched off.
        void* mini = g_lastMiniRoot.load();
        if (gs::Settings::Get().miniPin && mini && g_orig[1])
        {
            struct { int64_t id; uint8_t kind; uint8_t pad[7]; } miniKey{
                100000 + static_cast<int64_t>(n), 0x15, {}};
            void* rm = g_orig[1](mini, &type, &miniKey, &dword4, &float5, pos, label, name,
                                 reinterpret_cast<void*>(static_cast<uintptr_t>(0)), struct10,
                                 reinterpret_cast<void*>(static_cast<uintptr_t>(1)),
                                 nullptr, nullptr, nullptr);
            GS_LOG("[pin #%llu] minimap copy key=%lld returned 0x%p",
                   static_cast<unsigned long long>(n), static_cast<long long>(miniKey.id), rm);
        }
        Remember(x, y, z, label, true, key.id);
        return r;
    }

    int LivePinKeys(int64_t* out, int n)
    {
        const int total = g_placedN.load();
        int got = 0;
        for (int i = 0; i < total && i < kMaxPins && got < n; ++i)
            if (!g_placed[i].gone) out[got++] = g_placed[i].id;
        return got;
    }

    void Remember(float x, float y, float z, const char* label, bool drawn, int64_t keyId)
    {
        const int i = g_placedN.load();
        if (i >= kMaxPins) return;
        g_placed[i].x = x; g_placed[i].y = y; g_placed[i].z = z;
        g_placed[i].drawn = drawn;
        g_placed[i].gone = false;
        g_placed[i].id = keyId;
        CopyString(g_placed[i].label, sizeof(g_placed[i].label), label ? label : "Marker");
        g_placedN.store(i + 1);
    }

    bool PinNear(float x, float z, float radius)
    {
        const int n = g_placedN.load();
        for (int i = 0; i < n && i < kMaxPins; ++i)
        {
            if (g_placed[i].gone) continue;
            const float dx = g_placed[i].x - x, dz = g_placed[i].z - z;
            if (dx * dx + dz * dz <= radius * radius) return true;
        }
        return false;
    }

    int PinCount() { return g_placedN.load(); }

    int LivePinsAtOrAbove(int64_t minId)
    {
        const int n = g_placedN.load();
        int live = 0;
        for (int i = 0; i < n && i < kMaxPins; ++i)
            if (!g_placed[i].gone && g_placed[i].id >= minId) ++live;
        return live;
    }

    int InstallRemoveSpy(uintptr_t worldVtable, uintptr_t miniVtable)
    {
        const uintptr_t vts[2] = {worldVtable, miniVtable};
        int n = 0;
        for (int i = 0; i < 2; ++i)
        {
            if (!vts[i]) continue;
            void* replacement = i == 0 ? reinterpret_cast<void*>(&DetourRemoveWorld)
                                       : reinterpret_cast<void*>(&DetourRemoveMini);
            if (!gs::vtable::Install(vts[i], gs::sig::kSlotRemoveIcon, replacement, g_rmSwap[i]))
            {
                GS_LOG_ERR("[rmspy] could not take slot %d on surface %d",
                           gs::sig::kSlotRemoveIcon, i);
                continue;
            }
            g_rmOrig[i] = reinterpret_cast<CreateFn>(g_rmSwap[i].original);
            ++n;
        }
        if (n) GS_LOG_OK("[rmspy] slot %d taken on %d surface(s). Delete a marker on the map and "
                         "the log says exactly what the game asked for.",
                         gs::sig::kSlotRemoveIcon, n);
        return n;
    }

    bool RepinWanted() { return g_repinWanted.load(); }

    void Repin(void* worldRoot)
    {
        g_repinWanted.store(false);
        const int n = g_placedN.load();
        if (n <= 0) return;
        if (!worldRoot || !g_orig[0] || !RootLooksRight(worldRoot))
        {
            GS_LOG("[pin] the map was rebuilt but there is no usable root, so %d pin(s) wait", n);
            g_repinWanted.store(true);
            return;
        }
        GS_LOG("[pin] the game rebuilt its icons; drawing this session's %d pin(s) again", n);
        for (int i = 0; i < n && i < kMaxPins; ++i)
        {
            // A real marker is in the game's own list and comes back with
            // everything else it rebuilt, and one the player deleted stays
            // deleted.
            if (!g_placed[i].drawn || g_placed[i].gone) continue;
            uint16_t type = 0x0001;
            struct { int64_t id; uint8_t kind; uint8_t pad[7]; } key{
                g_placed[i].id != 0 ? g_placed[i].id : 2000 + static_cast<int64_t>(i), 0x15, {}};
            uint32_t dword4 = 0;
            float float5 = 0.0f;
            float pos[3] = {g_placed[i].x, 0.0f, g_placed[i].z};
            char label[48];
            CopyString(label, sizeof(label), g_placed[i].label);
            char name[64] = "MapIcon_Pin_Marker";
            uint8_t struct10[36]{};
            g_orig[0](worldRoot, &type, &key, &dword4, &float5, pos, label, name,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(0)), struct10,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(1)),
                      nullptr, nullptr, nullptr);
        }
    }

    bool RemoveIcon(void* worldRoot, int64_t keyId)
    {
        if (!worldRoot || !g_rmOrig[0] || !RootLooksRight(worldRoot)) return false;
        struct { int64_t id; uint8_t kind; uint8_t pad[7]; } key{keyId, 0x15, {}};
        g_rmOrig[0](worldRoot, reinterpret_cast<void*>(static_cast<uintptr_t>(0x0001)), &key,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(0)), nullptr, nullptr, nullptr,
                    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        void* mini = g_lastMiniRoot.load();
        if (mini && g_rmOrig[1])
            g_rmOrig[1](mini, reinterpret_cast<void*>(static_cast<uintptr_t>(0x0001)), &key,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(0)), nullptr, nullptr,
                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        return true;
    }

    bool Forget(int64_t keyId, float* x, float* z)
    {
        const int n = g_placedN.load();
        for (int i = 0; i < n && i < kMaxPins; ++i)
        {
            if (g_placed[i].gone || g_placed[i].id != keyId) continue;
            g_placed[i].gone = true;
            if (x) *x = g_placed[i].x;
            if (z) *z = g_placed[i].z;
            GS_LOG_OK("[pin] the game removed the mod's \"%s\" pin, key %lld; that place is free "
                      "to mark again", g_placed[i].label, static_cast<long long>(keyId));
            return true;
        }
        return false;
    }

    void ForgetAll()
    {
        g_placedN.store(0);
        g_repinWanted.store(false);
    }

    void* LastWorldRoot() { return g_lastWorldRoot.load(); }
    void* LastMiniRoot() { return g_lastMiniRoot.load(); }

    bool RequestReplay()
    {
        if (!g_swap[0].installed || !g_orig[0])
        {
            GS_LOG_ERR("[key] the world map spy is not installed, no replay");
            return false;
        }
        const bool already = g_replayPending.exchange(true);
        GS_LOG(already
               ? "[key] still armed. Place a custom marker on the world map and a second one follows it."
               : "[key] armed. Place a custom marker on the world map; right after the game places it, a copy goes 30 units away.");
        return true;
    }
}
