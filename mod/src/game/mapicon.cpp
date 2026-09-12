#include "game/mapicon.h"

#include <Windows.h>
#include <intrin.h>
#include <atomic>
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
    struct Placed { float x, z; };
    Placed g_placed[kMaxPins];
    std::atomic<int> g_placedN{0};

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
    int g_pinCallerLeft = 6;

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

    void Record(int surface, void* const* a)
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
        // is what the mod has to write into for Seth to be able to delete a
        // pin. Session eighty-one hunted for the list by the coordinates it
        // must contain and found a position trail instead, because he happens
        // to stand near one of his own markers. A caller cannot be
        // coincidence.
        if (pin && g_pinCallerLeft > 0)
        {
            --g_pinCallerLeft;
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            GS_LOG("[spy] a MapIcon_Pin_Marker create was called from +0x%08llX (surface %d, key %lld)",
                   static_cast<unsigned long long>(ret - base), surface,
                   static_cast<long long>(c.keyId));
        }
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

    void* DetourWorld(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                      void* a8, void* a9, void* a10, void* a11, void* a12, void* a13, void* a14)
    {
        void* a[14] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14};
        Record(0, a);
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

    void* PlacePinNow(void* worldRoot, float x, float y, float z, const char* labelText)
    {
        if (!worldRoot || !g_orig[0])
        {
            GS_LOG_ERR("[pin] no world root or no original slot 170, nothing placed");
            return nullptr;
        }
        const uint64_t n = ++g_replayCount;

        // Session ten, byte for byte, except the position and the key id.
        uint16_t type = 0x0001;
        struct { int64_t id; uint8_t kind; uint8_t pad[7]; } key{1000 + static_cast<int64_t>(n), 0x15, {}};
        uint32_t dword4 = 0;
        float float5 = 0.0f;
        // Height zero, because that is what the game passes for this icon.
        // Session forty-seven caught it creating a Pin_Marker of its own when
        // Seth placed a marker by hand: name "MapIcon_Pin_Marker", type
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

        // The same icon on the minimap, only when the ini asks.
        //
        // Its own key id, a hundred thousand above the world map's. The two
        // calls shared one id in 0.36.0, and if the game keys icons by id
        // alone then the second call was not adding a copy but moving the
        // first one onto the other surface, which would take the pin off the
        // map Seth was looking at. That is a guess about why a working
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
        // And say so, because the map is not on screen when this happens.
        if (gs::Settings::Get().rumble) gs::pad::Buzz(28000, 220);

        const int i = g_placedN.load();
        if (i < kMaxPins) { g_placed[i] = {x, z}; g_placedN.store(i + 1); }
        return r;
    }

    bool PinNear(float x, float z, float radius)
    {
        const int n = g_placedN.load();
        for (int i = 0; i < n && i < kMaxPins; ++i)
        {
            const float dx = g_placed[i].x - x, dz = g_placed[i].z - z;
            if (dx * dx + dz * dz <= radius * radius) return true;
        }
        return false;
    }

    int PinCount() { return g_placedN.load(); }

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
