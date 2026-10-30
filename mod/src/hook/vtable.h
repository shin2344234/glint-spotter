#pragma once
#include <cstdint>

// Replacing one slot in a vtable, and putting it back.
//
// This is the first thing in the plugin that writes to the game. It changes
// eight bytes: one function pointer in one vtable. Every instance of that class
// then calls the replacement, which is exactly why a spy on slot 174 needs no
// object pointer to install, only the vtable, and the vtable is RTTI-verified
// before this is ever called.
//
// Whatever was in the slot is handed back so the replacement can forward to it.
// That is how two mods stack on one slot, provided the other one goes first.
// Crimson Route hooks the update slot (35 through 2976, 36 on 3392) of these
// same vtables only while the slot still
// holds the game's own function. A swap made before Route's leaves Route no
// slot at all, and it turns its map drawing off; that was every launch on 23
// September. So the worker holds the update swaps until Route has hooked
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

    // True when fn is the stub the compiler fills unused slots with, a bare
    // ret 0 (C2 00 00). On 3392 the map roots' slots 170 and 171 and the alert
    // root's 144 became that stub when the classes gained slots, so a slot
    // index that has gone stale after a patch usually lands on it.
    bool IsEmptySlot(const void* fn);

    // Write replacement into vtable[index]. Returns false and leaves the vtable
    // untouched if the page cannot be made writable, if the slot already
    // holds the replacement, or if the slot holds the empty stub above, since
    // hooking that would forward every call to a function that does nothing. When publish is given, the slot's old value goes
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
