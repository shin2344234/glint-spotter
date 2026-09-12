#pragma once
#include <cstddef>
#include <cstdint>

// A jump written over the front of a game function.
//
// Everything else this mod hooks is a vtable slot, which is one pointer swap
// and reversible by writing the old value back. That covers anything the game
// reaches through an object. It does not cover a plain function, and after a
// hundred and five sessions there is exactly one question left that only a
// plain function can answer: when the player places a marker by hand, does the
// game call the create at 0x27FE130, and if it does, on which object?
//
// The mod has been writing records into the marker list hanging off the player
// it found, and the marker the player places by hand does not appear there.
// Either the game uses a different object of the same shape, in which case
// using that one makes every mod pin a real marker, or it does not use this
// function at all, in which case the whole approach is finished. A log line
// from inside the function settles it either way.
//
// The mechanics, kept as small as they can be:
//   - the first `len` bytes of the target are copied to a trampoline
//   - the trampoline ends with an absolute jump back to target + len
//   - the target starts with an absolute jump to the detour
//
// An absolute jump is `FF 25 00 00 00 00` followed by the eight byte address,
// fourteen bytes, and it needs no reachable range. `len` must be at least
// fourteen and must land on an instruction boundary, so the caller passes a
// length it read off the disassembly rather than a length this file guesses.
// The bytes copied must also be position independent, which is true of the two
// prologues this is used on: register spills to the stack frame and pushes.

namespace gs::inlinehook
{
    struct Hook
    {
        uintptr_t target = 0;
        void* trampoline = nullptr;   // call this to run the original
        uint8_t saved[32]{};
        size_t len = 0;
        bool installed = false;
    };

    // `expect` is the prologue as the analysis read it, and the install is
    // refused if the running game does not match it byte for byte.
    bool Install(Hook& h, uintptr_t target, size_t len, void* detour,
                 const uint8_t* expect, size_t expectLen);

    // Puts the original bytes back. Safe to call on a hook that never went in.
    void Remove(Hook& h);
}
