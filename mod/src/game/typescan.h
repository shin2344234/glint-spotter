#pragma once
#include <cstdint>

// Finding classes in the running game by name, through its RTTI.
//
// signatures.h names two vtables by address, which is enough to answer one
// question and nothing else, and it goes stale on the next patch. This walks the
// image instead: every complete object locator in it, the decorated name behind
// each one, and the vtable that points back at it. Ask for a keyword and get
// every class whose name contains it, with a live vtable address, on whatever
// build happens to be running.
//
// The point is coverage. A session costs the player real time, so the probe
// should come back with the whole map icon family and not just the two classes
// that were interesting when it was written.

namespace gs::typescan
{
    struct ClassInfo
    {
        char name[224]{};      // decorated, ".?AVFoo@bar@pa@@"
        uintptr_t colVa = 0;   // its complete object locator
        uintptr_t vtableVa = 0;// 0 when no vtable points at the locator
        uint32_t vtableRva = 0;
        int slots = 0;         // how many plausible function pointers follow
    };

    // Every class whose decorated name contains any of the keywords, case
    // sensitive. Two passes over the module image, so call it once and keep the
    // answer. Returns how many were written to out.
    size_t FindClasses(const char* const* keywords, size_t keywordCount,
                       ClassInfo* out, size_t capacity);

    // Where the module starts and how far it runs, for callers that want to log it.
    bool ModuleRange(uintptr_t& base, size_t& size);
}
