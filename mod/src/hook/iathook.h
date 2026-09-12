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
}
