#pragma once

// One entry in the game's import table, pointed somewhere else.
//
// The other two ways this mod redirects a call are a vtable slot, which needs
// an object, and a jump written over a function, which needs the first bytes
// of that function to be the bytes the analysis read. Neither fits a Windows
// import: there is no object, and the function belongs to kernel32, which is
// not the image this mod checks for patches.
//
// An import is simpler than both. The loader writes the real address into a
// table of pointers in the exe, and every call site reads that table, so one
// pointer written by hand redirects all of them. Nothing is patched, no code
// is copied, and the entry can be put back exactly as it was.
//
// The catch is that it only catches calls made through the table. A call the
// game makes after resolving the address itself is invisible here.

namespace gs::iathook
{
    // Point the named import at `detour`. `dll` is matched without case, `fn`
    // exactly, both as they appear in the import table. False when the image
    // does not import it, or when the entry will not take a write.
    //
    // `previous` is filled with whatever the entry held, and it is filled
    // before the detour goes in, because the moment it does another thread can
    // already be inside the detour looking for something to forward to.
    bool Swap(const char* dll, const char* fn, void* detour, void** previous);

    // The same for an import that may be listed more than once, or brought in
    // by ordinal with no name to match. Exe 1.0.0.3576 lists XINPUT1_4.dll
    // twice: once in the main table, by ordinal 3 for XInputSetState, and
    // again as xinput1_4.dll, also by ordinal, with the slot in .reloc and the
    // only caller inside the protector's code. Swap stops at the first match
    // and skips ordinals, so it reaches neither.
    //
    // Every entry under a descriptor named `dll` (any case) that holds
    // `ordinal`, or a function called `fn` when `fn` is not null, is pointed at
    // `detour`. `previous` gets the value the first entry held, before any
    // write. An entry holding something else is left alone, since forwarding
    // it to `previous` would send its calls to the wrong place, and counted in
    // `differs`. Up to `max` entry addresses go in `where`, for Restore and
    // for checking later that the detour is still in them. Returns how many
    // entries took the write.
    int SwapAll(const char* dll, unsigned ordinal, const char* fn, void* detour, void** previous,
                void** where[], int max, int* differs);

    // Write `original` back into each entry in `where` that still holds
    // `detour`.
    void Restore(void** const where[], int n, void* detour, void* original);
}
