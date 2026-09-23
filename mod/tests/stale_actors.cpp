// The entity set reading blocks the game has freed.
//
// Master Looter's fault handler caught five access violations inside
// actors.cpp across three of Seth's sessions on 16 September 2026, all of them
// within half a minute of a teleport, in EntityLike, WorldPos, GlintByte and
// ReadLit. Every one was caught by the __try around the read, so nothing
// crashed. The half that does not fault is the problem: when the game has
// already put something else in the freed block, the read succeeds and returns
// that something else. The same morning's log has the set holding an object
// 5,240 metres away whose glint byte read as set.
//
// These drive the real actors.cpp with heap blocks standing in for the game's
// components, through the sequence a load puts them through.
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

// The log is counted, not printed: the set being dropped is a log line, and
// how many times it happens is what one of these tests is about.
int g_drops = 0;
namespace gs::Log
{
    void Write(const char* level, const char* fmt, ...)
    {
        (void)level;
        if (fmt && strstr(fmt, "are dropped")) ++g_drops;
    }
}

// actors.cpp and rtti.cpp reach for a few things outside themselves. None
// of them is what is under test, so they answer plainly.
namespace gs::player
{
    struct Pos;
    Pos Read();
}
namespace gs::aim
{
    bool FlashActive() { return false; }
}
namespace gs::typescan
{
    bool ModuleRange(uintptr_t& base, size_t& size) { base = 0; size = 0; return false; }
    int FindGlobals(uintptr_t, uintptr_t*, int) { return 0; }
}
namespace gs::Settings
{
    bool Marked(const char*) { return false; }
}

#include "core/load.cpp"
#include "game/rtti.cpp"
#include "game/actors.cpp"

// Where the tests say the player is. Invalid until one of them says otherwise.
gs::player::Pos g_player;
namespace gs::player
{
    Pos Read() { return g_player; }
}

namespace
{
    int failures = 0;

    void Expect(const char* what, bool ok)
    {
        printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++failures;
    }

    // A component block: its vtable first, then the bytes the mod reads out of
    // it. Big enough to hold both offsets this file uses.
    constexpr size_t kBlock = 0x400;
    constexpr uintptr_t kVtable = 0x1454783B8ull;   // the gimmick component's, as a value
    constexpr uintptr_t kOther  = 0x1454778F8ull;   // whatever moved in afterwards

    uint8_t* NewBlock(uintptr_t vt)
    {
        auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, kBlock, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        memset(p, 0, kBlock);
        *reinterpret_cast<uintptr_t*>(p) = vt;
        return p;
    }
}

int main()
{
    printf("-- the addresses that faulted are not pointers --\n");
    // The exact shapes Master Looter's handler caught, minus the field offset
    // that was added to them: a 32-bit value in the high half and nothing in
    // the low half.
    Expect("0000045A_00000000 is refused", !PtrLike(0x0000045A00000000ull));
    Expect("00001002_00000000 is refused", !PtrLike(0x0000100200000000ull));
    Expect("00000005_00000000 is refused", !PtrLike(0x0000000500000000ull));
    Expect("a real heap pointer is still accepted", PtrLike(0x0000045AEDA61C00ull));
    Expect("a pointer whose low half is only just set is accepted", PtrLike(0x0000045A00000008ull));
    Expect("null is refused", !PtrLike(0));
    Expect("an unaligned pointer is refused", !PtrLike(0x0000045AEDA61C01ull));
    Expect("a kernel address is refused", !PtrLike(0xFFFF800000001000ull));

    printf("-- the startup probe over pool slots --\n");
    // EntityLike is what faulted 13 seconds into every session, reading +0x60
    // of a slot that held two 32-bit values rather than a pointer.
    Expect("the slot that faulted is refused without reading it",
           !EntityLike(0x0000045A00000000ull));
    uint8_t* ent = NewBlock(0);
    *reinterpret_cast<uint32_t*>(ent + 0x60) = 0xB0100996;   // a world entity's id
    Expect("a real entity block is still recognised", EntityLike(reinterpret_cast<uintptr_t>(ent)));
    *reinterpret_cast<uint32_t*>(ent + 0x60) = 0x10100996;   // not a tag the mod knows
    Expect("a block with the wrong tag is refused", !EntityLike(reinterpret_cast<uintptr_t>(ent)));

    printf("-- a component that is still itself --\n");
    uint8_t* comp = NewBlock(kVtable);
    const uintptr_t c = reinterpret_cast<uintptr_t>(comp);
    comp[gs::sig::kOff_Gimmick_DetectTgt] = 1;
    bool glint = false;
    Expect("its vtable matches", StillTheSame(c, kVtable));
    Expect("the glint byte is read", GlintByte(c, kVtable, &glint) && glint);
    comp[gs::sig::kOff_Gimmick_DetectTgt] = 0;
    Expect("and read again when it clears", GlintByte(c, kVtable, &glint) && !glint);

    printf("-- the block is freed and something else moves in --\n");
    comp[gs::sig::kOff_Gimmick_DetectTgt] = 1;      // whatever the new owner keeps there
    *reinterpret_cast<uintptr_t*>(comp) = kOther;   // its vtable, not ours
    glint = false;
    Expect("the component is no longer itself", !StillTheSame(c, kVtable));
    Expect("so the glint byte is refused rather than believed", !GlintByte(c, kVtable, &glint));
    Expect("and nothing was written to the caller's answer", !glint);

    printf("-- the block is decommitted outright --\n");
    uint8_t* gone = NewBlock(kVtable);
    const uintptr_t g = reinterpret_cast<uintptr_t>(gone);
    Expect("readable while it is there", StillTheSame(g, kVtable));
    VirtualFree(gone, 0, MEM_RELEASE);
    Expect("refused once the page is gone, with no fault", !StillTheSame(g, kVtable));
    bool any = false;
    Expect("and the glint byte is not read through it", !GlintByte(g, kVtable, &any));

    printf("-- the reveal, through either component --\n");
    uint8_t* detect = NewBlock(kVtable);
    const uintptr_t d = reinterpret_cast<uintptr_t>(detect);
    detect[gs::sig::kOff_Detect_Lit] = 1;
    bool lit = false;
    Expect("read off the detect component", ReadLit(d, kVtable, 0, 0, &lit) && lit);
    *reinterpret_cast<uintptr_t*>(detect) = kOther;
    lit = false;
    Expect("a stale detect component reads nothing", !ReadLit(d, kVtable, 0, 0, &lit) && !lit);

    // With the detect component stale, a live gimmick component still answers:
    // ReadLit drops the one it cannot trust rather than giving up on both.
    uint8_t* gim = NewBlock(kVtable);
    const uintptr_t gc = reinterpret_cast<uintptr_t>(gim);
    uint8_t* sub = NewBlock(0);
    *reinterpret_cast<uint32_t*>(sub + gs::sig::kOff_GimmickSub_Active) = 1;
    *reinterpret_cast<uintptr_t*>(gim + gs::sig::kOff_Gimmick_Sub) = reinterpret_cast<uintptr_t>(sub);
    lit = false;
    Expect("the gimmick component answers when the detect one is stale",
           ReadLit(d, kVtable, gc, kVtable, &lit) && lit);
    *reinterpret_cast<uintptr_t*>(gim) = kOther;
    lit = false;
    Expect("and when both are stale, nothing is read", !ReadLit(d, kVtable, gc, kVtable, &lit) && !lit);

    printf("-- a sub-object pointer of the shape that faulted --\n");
    uint8_t* gim2 = NewBlock(kVtable);
    const uintptr_t gc2 = reinterpret_cast<uintptr_t>(gim2);
    *reinterpret_cast<uintptr_t*>(gim2 + gs::sig::kOff_Gimmick_Sub) = 0x0000100200000000ull;
    lit = true;
    Expect("ReadLit refuses it instead of reading 0x1B8 off it",
           !ReadLit(0, 0, gc2, kVtable, &lit));

    printf("-- a pool walk that runs off the end of its allocation --\n");
    // Three pages reserved, the first and third committed, the middle one not.
    // Pool A starts on the first page with four entities and nothing after
    // them, so the walk is still counting empty slots when it reaches the
    // unmapped page. Pool B starts on the third page. On 16 September the walk
    // read straight into the gap, the fault ended the pass, and pool B was
    // never read.
    {
        const size_t page = 0x1000;
        auto* region = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 3, MEM_RESERVE, PAGE_NOACCESS));
        VirtualAlloc(region, page, MEM_COMMIT, PAGE_READWRITE);
        VirtualAlloc(region + page * 2, page, MEM_COMMIT, PAGE_READWRITE);
        memset(region, 0, page);
        memset(region + page * 2, 0, page);

        uint8_t* one = NewBlock(0);
        *reinterpret_cast<uint32_t*>(one + 0x60) = 0xB0100001;
        const uintptr_t entity = reinterpret_cast<uintptr_t>(one);
        auto* poolA = reinterpret_cast<uintptr_t*>(region);
        auto* poolB = reinterpret_cast<uintptr_t*>(region + page * 2);
        for (int i = 0; i < 4; ++i) { poolA[i] = entity; poolB[i] = entity; }

        // The manager: a page of zeros with the two pool pointers past the
        // {count, capacity, array} triples, where the pool scan looks.
        uint8_t* mgr = NewBlock(0);
        *reinterpret_cast<uintptr_t*>(mgr + 0x208) = reinterpret_cast<uintptr_t>(poolA);
        *reinterpret_cast<uintptr_t*>(mgr + 0x210) = reinterpret_cast<uintptr_t>(poolB);

        uintptr_t got[64];
        const int n = ReadPools(reinterpret_cast<uintptr_t>(mgr), got, 64);
        printf("      ReadPools returned %d\n", n);
        Expect("both pools are read, the one past the gap included", n == 8);
    }

    printf("-- the player flips to the loading placeholder and back --\n");
    // 16 September, 11:13 to 11:16: standing about 11,171 metres from the
    // origin, the player read flipped between his real position and the
    // placeholder near (0, 0) every pass or two, and the set was dropped each
    // time as though he had teleported 11 km. A real fast travel has to still
    // drop it, once.
    {
        constexpr float kX = -11114.3f, kY = 700.0f, kZ = -1128.5f;
        const int kEntities = 4;

        // An entity the set will keep: its id tagged as a world entity, and a
        // transform with a cached world position a few metres from the player.
        // The local position is left insane so it offers no second candidate.
        uintptr_t ents[kEntities];
        for (int i = 0; i < kEntities; ++i)
        {
            uint8_t* e = NewBlock(0);
            uint8_t* comps = NewBlock(0);
            uint8_t* tf = NewBlock(0);
            *reinterpret_cast<uint32_t*>(e + 0x60) = 0xB0100100 + i;
            *reinterpret_cast<uintptr_t*>(e + 0x68) = reinterpret_cast<uintptr_t>(comps);
            *reinterpret_cast<uintptr_t*>(comps + 0x1A0) = reinterpret_cast<uintptr_t>(tf);
            const float world[3] = {kX + 5.0f * i, kY, kZ + 3.0f};
            memcpy(tf + 0x29C, world, 12);
            const float nan[3] = {1.0e30f, 1.0e30f, 1.0e30f};
            memcpy(tf + 0xB4, nan, 12);
            *reinterpret_cast<uint32_t*>(tf + 0xC8) = 0xFFFFFFFF;
            ents[i] = reinterpret_cast<uintptr_t>(e);
        }
        uint8_t* pool = NewBlock(0);
        for (int i = 0; i < kEntities; ++i) reinterpret_cast<uintptr_t*>(pool)[i] = ents[i];
        uint8_t* mgr = NewBlock(0);
        *reinterpret_cast<uintptr_t*>(mgr + 0x208) = reinterpret_cast<uintptr_t>(pool);
        g_mgr.store(reinterpret_cast<uintptr_t>(mgr));

        auto at = [](float x, float y, float z) {
            gs::player::Pos p{};
            p.x = x; p.y = y; p.z = z;
            p.valid = true;
            return p;
        };
        const gs::player::Pos home = at(kX, kY, kZ);
        const gs::player::Pos placeholder = at(-0.3f, 1.0f, 0.0f);

        uint32_t now = 100000;
        g_player = home;
        gs::actors::Refresh(now += 500);
        Expect("the set fills where the player stands", gs::actors::Count() == kEntities);

        g_drops = 0;
        bool heldEveryPass = true;
        for (int flip = 0; flip < 20; ++flip)
        {
            g_player = (flip % 2 == 0) ? placeholder : home;
            gs::actors::Refresh(now += 500);
            if (gs::actors::Count() != kEntities) heldEveryPass = false;
        }
        printf("      twenty flips dropped the set %d time(s)\n", g_drops);
        Expect("twenty flips to the placeholder and back drop nothing", g_drops == 0);
        Expect("and the set keeps its entities through every one of them", heldEveryPass);

        gs::actors::Entity snap[16];
        const int got = gs::actors::Snapshot(snap, 16);
        bool noneAtOrigin = true;
        for (int i = 0; i < got; ++i)
            if (std::fabs(snap[i].x) + std::fabs(snap[i].z) < 100.0f) noneAtOrigin = false;
        Expect("no entity was placed at the origin during the placeholder passes", noneAtOrigin);

        // A real fast travel, three kilometres, straight from one real
        // position to another.
        g_drops = 0;
        g_player = at(kX + 3000.0f, kY, kZ);
        gs::actors::Refresh(now += 500);
        Expect("a real three-kilometre fast travel still drops the set once", g_drops == 1);
        Expect("and the old place's entities are not taken back in", gs::actors::Count() == 0);

        // Through the placeholder on the way, which is how a load looks.
        g_player = home;
        gs::actors::Refresh(now += 500);
        g_drops = 0;
        g_player = placeholder;
        gs::actors::Refresh(now += 500);
        g_player = at(kX - 4000.0f, kY, kZ);
        gs::actors::Refresh(now += 500);
        Expect("a load that passes through the placeholder drops it exactly once", g_drops == 1);

        g_mgr.store(0);
    }

    printf(failures ? "\n%d FAILED\n" : "\nall pass\n", failures);
    return failures ? 1 : 0;
}
