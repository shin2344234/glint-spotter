#pragma once
#include <cstdint>

// The list the game draws its own map markers from.
//
// The chain and the layout are no longer guesses. Both were read straight off
// the disassembly of the two functions that use them, and the arithmetic in
// the Upsert prologue is unambiguous:
//
//   movzx edi, r8b        ; the icon kind
//   shl   rdi, 4          ; times sixteen
//   add   rcx, 0xC8       ; the submodule
//   add   rdi, rcx        ; the header for that kind
//   mov   r8d, [rdi + 8]  ; how many
//   mov   r9,  [rdi]      ; where
//   lea   rdx, [rcx+rcx*2]
//   cmp   [r9 + rdx*8], rsi   ; stride of twenty-four, id first
//
// and its caller in the network Ack reaches the submodule as
// *(*(actor + 0x68) + 0x168), which is a ClientSelfContentsMiscActorComponent
// at runtime.
//
// So this build stops hunting. Session eighty-nine spent five hundred and
// eighty milliseconds per press sweeping memory for something whose address
// was computable all along, on the thread drawing the frame, which is the
// freeze I have been living with.

namespace gs::pinmodel
{
    // The submodule off the player, or zero.
    uintptr_t Submodule();

    struct List
    {
        uintptr_t header = 0;   // submodule + 0xC8 + kind * 16
        uintptr_t data = 0;     // records, 24 bytes each
        uint32_t count = 0;
        bool ok = false;
    };

    // Read one list. List 0 holds the player's markers and list 1 the traced
    // one; every other list belongs to something else and most are empty.
    List Read(int kind);

    // The same, off an object given rather than found. The game keeps its
    // markers in one of these hanging off something the mod's own actor
    // search does not reach, so once that object is known every read has
    // to go to it instead.
    List ReadAt(uintptr_t submodule, int kind);

    // Which lists have anything in them. Microseconds.
    void LogState(const char* why);
}
