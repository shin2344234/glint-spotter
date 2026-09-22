#pragma once
#include <cstdint>

// The live watch: which code turns a glint on, and on what.
//
// Reading the exe has found the functions that set the flash's reveal state
// three builds running, and never the call that turns it on: every static
// caller of the detect component's toggle passes "off". The notes all end
// the same way, that the next step is a live watchpoint. This is one.
//
// The first watch, on 22 September, set execute breakpoints on four
// functions the notes pointed at: the detect component's reveal toggle, the
// gimmick's lighting enable, the DetectLighting handler and the detect mode
// setter. None of them is how the flash lights a glint (re-glint-2944.md).
//
// This one watches the flash flag itself: a read or write data breakpoint on
// +0x40 of the special mode component, and on +0x50, which the same clear
// routine resets. Whatever reads the flag when the flash goes up is the code
// that reacts to it. Each instruction is recorded once by a vectored
// exception handler, with every register it held with the flash off and
// again with it up, and the key thread names those registers against the
// entity set and the game's classes.
//
// Off unless the ini says Watch=1. It is a diagnostic and never ships on.
namespace gs::watch
{
    // Arms the breakpoints on every thread that exists, once the addresses
    // check out. Safe to call again: threads that appeared since are armed.
    void Arm();
    // Logs what the handler caught since the last call.
    void Drain();
    // Clears the debug registers on every thread it armed.
    void Disarm();
}
