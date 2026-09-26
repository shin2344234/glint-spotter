// The flash flag through a save load.
//
// The flag is a dword at +0x40 of the player's special mode component, and
// the aim module reads it straight out of the pointer it was handed. A load
// frees that object. LuxDragon's 1.1.20 log has the block reused by something
// else with 0xFFFFFFFF where the flag was, the flash reading as on for the
// fourteen minutes that followed, and the automatic marker pinning whatever
// the camera settled on during a conversation. These drive the real aim.cpp
// through that sequence with heap blocks standing in for the game's objects.
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gs::Log
{
    void Write(const char* level, const char* fmt, ...) { (void)level; (void)fmt; }
}

// aim.cpp asks whether a pointer is in the entity set; nothing here is.
namespace gs::actors
{
    bool InSet(uintptr_t) { return false; }
}

namespace gs::load
{
    void CountQuery() {}
}

#include "game/rtti.cpp"
#include "game/aim.cpp"

namespace
{
    int failures = 0;

    void Expect(const char* what, bool ok)
    {
        printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++failures;
    }

    constexpr uintptr_t kVtable = 0x14547EAC8ull;   // the class's vtable on 2850, as a value
    constexpr uintptr_t kOther  = 0x1454778F8ull;   // what LuxDragon's block carried afterwards
    constexpr uintptr_t kFlagOn = 0xA0100006;       // the player's actor id

    struct Fake
    {
        alignas(16) uint8_t bytes[0x100]{};
        uintptr_t at() const { return reinterpret_cast<uintptr_t>(bytes); }
        void Shape(uintptr_t vt, uintptr_t owner)
        {
            memcpy(bytes + 0x00, &vt, sizeof(vt));
            memcpy(bytes + 0x08, &owner, sizeof(owner));
        }
        void Flag(uint32_t v) { memcpy(bytes + 0x40, &v, sizeof(v)); }
        void Mode(uint16_t id) { memcpy(bytes + 0x30, &id, sizeof(id)); }
    };
}

int main()
{
    static Fake special;
    static uint8_t bodyA[0x80], bodyB[0x80];
    const uintptr_t actorA = reinterpret_cast<uintptr_t>(bodyA);
    const uintptr_t actorB = reinterpret_cast<uintptr_t>(bodyB);

    printf("-- the component as handed over at startup --\n");
    special.Shape(kVtable, actorA);
    special.Flag(0);
    gs::aim::SetPlayerActor(actorA);
    gs::aim::SetSpecialComponent(special.at());
    Expect("a zero flag reads as off", !gs::aim::FlashActive());
    special.Flag(kFlagOn);
    Expect("the player's id in the flag reads as on", gs::aim::FlashActive());
    special.Flag(0);
    Expect("cleared again reads as off", !gs::aim::FlashActive());
    Expect("the component is reported", gs::aim::SpecialComponent() == special.at());

    printf("-- the load: the block is freed and reused by something else --\n");
    special.Shape(kOther, 0x5D5935D67E0ull);
    special.Flag(0xFFFFFFFF);
    Expect("a reused block reads as off however the flag looks", !gs::aim::FlashActive());
    Expect("and the component is dropped", gs::aim::SpecialComponent() == 0);
    special.Flag(kFlagOn);
    Expect("and stays off even with the player's id in the flag", !gs::aim::FlashActive());

    printf("-- found again --\n");
    special.Shape(kVtable, actorA);
    special.Flag(kFlagOn);
    gs::aim::SetSpecialComponent(special.at());
    Expect("handed over again it reads as on", gs::aim::FlashActive());

    printf("-- which mode raised the flag (GitHub #1); the component holds the mode's row --\n");
    special.Mode(8);
    Expect("before the game's table is read, Knowledge still counts, as it always did", gs::aim::FlashActive());
    gs::aim::SetModeTableRows(26);
    Expect("with the table read, Knowledge (row 8) with the flag up reads as off", !gs::aim::FlashActive());
    special.Mode(7);
    Expect("so does Anamorphic (row 7)", !gs::aim::FlashActive());
    special.Mode(6);
    Expect("SwordFlash (row 6) reads as on", gs::aim::FlashActive());
    special.Mode(3);
    Expect("Detect_Lantern (row 3, key 103) reads as on", gs::aim::FlashActive());
    special.Mode(0);
    Expect("plain Detect (row 0) reads as on", gs::aim::FlashActive());
    special.Mode(15);
    Expect("DetectTaeguk (row 15) reads as on", gs::aim::FlashActive());
    special.Mode(0x1234);
    Expect("a row past the table reads as on, so a new mode cannot switch the marker off", gs::aim::FlashActive());
    special.Mode(0xFFFF);
    Expect("an empty record reads as on", gs::aim::FlashActive());
    special.Mode(8);
    special.Flag(0);
    Expect("Knowledge with the flag down reads as off", !gs::aim::FlashActive());
    special.Flag(kFlagOn);
    gs::aim::SetModeTableRows(27);
    Expect("a table with a different row count turns the check off, so Knowledge counts again", gs::aim::FlashActive());
    gs::aim::SetModeTableRows(26);
    special.Mode(0);
    Expect("names come from the game's table by row", gs::aim::ModeName(6) && strcmp(gs::aim::ModeName(6), "SwordFlash") == 0 &&
                                                         gs::aim::ModeName(8) && strcmp(gs::aim::ModeName(8), "Knowledge") == 0 &&
                                                         gs::aim::ModeName(26) == nullptr);

    printf("-- the load again, but the block is left intact --\n");
    gs::aim::SetPlayerActor(actorB);
    Expect("the old body's component reads as off once the player is a new body", !gs::aim::FlashActive());
    Expect("but it is kept, because the actor may be about to catch up", gs::aim::SpecialComponent() == special.at());
    gs::aim::SetPlayerActor(actorA);
    Expect("and reads as on again when the actor is its owner", gs::aim::FlashActive());

    printf("-- the block is reused by another character's component of the same class --\n");
    special.Shape(kVtable, actorB);
    Expect("a different owner reads as off", !gs::aim::FlashActive());
    Expect("and the component is dropped", gs::aim::SpecialComponent() == 0);

    printf("-- the worker takes it away --\n");
    special.Shape(kVtable, actorA);
    gs::aim::SetSpecialComponent(special.at());
    Expect("on while it is held", gs::aim::FlashActive());
    gs::aim::SetSpecialComponent(0);
    Expect("off once it is cleared", !gs::aim::FlashActive());
    Expect("and nothing is reported", gs::aim::SpecialComponent() == 0);

    printf("-- the block is decommitted outright --\n");
    void* page = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Expect("a page could be had for the test", page != nullptr);
    if (page)
    {
        const uintptr_t sp = reinterpret_cast<uintptr_t>(page);
        memcpy(page, &kVtable, sizeof(kVtable));
        memcpy(static_cast<uint8_t*>(page) + 0x08, &actorA, sizeof(actorA));
        const uint32_t on = kFlagOn;
        memcpy(static_cast<uint8_t*>(page) + 0x40, &on, sizeof(on));
        gs::aim::SetSpecialComponent(sp);
        Expect("on while the page is there", gs::aim::FlashActive());
        VirtualFree(page, 0, MEM_RELEASE);
        Expect("off once the page is gone, with no fault", !gs::aim::FlashActive());
        Expect("and the component is dropped", gs::aim::SpecialComponent() == 0);
    }

    printf("-- a handover of something unreadable --\n");
    gs::aim::SetSpecialComponent(0x10);
    Expect("is refused rather than kept", gs::aim::SpecialComponent() == 0 && !gs::aim::FlashActive());

    if (failures)
    {
        printf("\n%d FAILED\n", failures);
        return 1;
    }
    printf("\nall pass\n");
    return 0;
}
