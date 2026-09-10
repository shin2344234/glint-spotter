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
// That is how two mods stack on one slot: Crimson Route's detour on slot 35 of
// these same vtables is the thing this must never trample, and it does not,
// because this only ever touches the slot it was asked for.

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
    // holds the replacement.
    bool Install(uintptr_t vtable, int index, void* replacement, Swap& out);

    // Put the original back, but only if the slot still holds our replacement.
    // If someone hooked on top of us after we installed, restoring would cut
    // them out, so the slot is left alone and the caller is told.
    bool Restore(Swap& s, bool& leftAlone);
}
