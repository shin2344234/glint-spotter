#pragma once
#include <cstdint>

// Everything read out of CrimsonDesert.exe, Steam build 2.01.00, exe 1.0.0.2760,
// on 10 September 2026. ImageBase is 0x140000000 and relocations are stripped, so
// an RVA here is also the runtime virtual address once the module base is added.
//
// A game patch moves all of it. Nothing below is dereferenced until rtti::VtableIs
// confirms the vtable names the class we expect, so a patched exe makes the
// feature refuse to install instead of reading a wild pointer. Re-derive with
// docs/investigations/vtres.py and xref.py.
//
// The full write-up is in FEASIBILITY.md.

namespace gs::sig
{
    // The two root UI controls that own every map icon. Slot 35 is the per frame
    // update, slot 170 creates an icon, and slot 172 is what the create path
    // calls on the way in.
    constexpr uintptr_t kWorldMapVtable = 0x0555CC70;
    constexpr uintptr_t kMiniMapVtable  = 0x0555DF70;

    constexpr const char* kWorldMapClass = ".?AVUIGamePlayControlRootWorldMap@uiCommonScript@pa@@";
    constexpr const char* kMiniMapClass  = ".?AVUIGamePlayControlRootMiniMap@uiCommonScript@pa@@";

    // Slot indices, identical on both classes.
    constexpr int kSlotUpdate     = 35;   // void __fastcall(this, float) on the world map
    constexpr int kSlotCreateIcon = 170;  // the map icon dispatcher

    // The dispatchers and constructors behind slot 170, kept for reference. The
    // probe does not call these; see FEASIBILITY.md for the argument list.
    constexpr uintptr_t kWorldMapCreateDispatch = 0x00D2F620;
    constexpr uintptr_t kWorldMapCreateCtor     = 0x00D2A250;
    constexpr uintptr_t kMiniMapCreateDispatch  = 0x00D801E0;
    constexpr uintptr_t kMiniMapCreateCtor      = 0x00D828E0;

    // Size the factory at 0x00E3CC40 allocates for a root control. Useful as a
    // sanity bound when a scan hit is checked.
    constexpr size_t kRootControlSize = 0xC48;
}
