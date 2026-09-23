#pragma once
#include <cstdint>

// Replacing one slot in a vtable, and putting it back.
//
// This is the first thing in the plugin that writes to the game. It changes
// eight bytes: one function pointer in one vtable. Every instance of that class
// then calls the replacement, which is exactly why a spy on slot 170 needs no
// object pointer to install, only the vtable, and the vtable is RTTI-verified
// before this is ever called.
//
// Whatever was in the slot is handed back so the replacement can forward to it.
// That is how two mods stack on one slot, provided the other one goes first.
// Crimson Route hooks slot 35 of these same vtables only while the slot still
// holds the game's own function. A swap made before Route's leaves Route no
// slot at all, and it turns its map drawing off; that was every launch on 23
// September. So the worker holds the slot 35 swaps until Route has hooked
// them (SlotThread in core/mod.cpp), and this then stacks on Route's detour.

namespace gs::vtable
{
    struct Swap
    {
        void** slot = nullptr;      // the vtable entry itself
        void* original = nullptr;   // what it held before us
        void* replacement = nullptr;
        bool installed = false;
    };

    // Write replacement into vtable[index]. Returns false and leaves the vtable
    // untouched if the page cannot be made writable, or if the slot already
    // holds the replacement. When publish is given, the slot's old value goes
    // there before the slot changes: a thunk that jumps through a global can
    // be called the instant the write lands if the game is already running
    // that slot.
    bool Install(uintptr_t vtable, int index, void* replacement, Swap& out,
                 void* volatile* publish = nullptr);

    // Put the original back, but only if the slot still holds our replacement.
    // If someone hooked on top of us after we installed, restoring would cut
    // them out, so the slot is left alone and the caller is told.
    bool Restore(Swap& s, bool& leftAlone);
}
