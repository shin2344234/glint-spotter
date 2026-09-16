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

namespace gs::Log
{
    void Write(const char* level, const char* fmt, ...) { (void)level; (void)fmt; }
}

// actors.cpp reaches for three things outside itself. None of them is what is
// under test, so they answer plainly.
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
}
namespace gs::Settings
{
    bool Marked(const char*) { return false; }
}

#include "game/rtti.cpp"
#include "game/actors.cpp"

namespace gs::player
{
    Pos Read() { return Pos{}; }
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

    printf(failures ? "\n%d FAILED\n" : "\nall pass\n", failures);
    return failures ? 1 : 0;
}
