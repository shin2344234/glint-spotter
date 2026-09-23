#pragma once
#include <cstdint>

// Reading MSVC RTTI out of the running game.
//
// The point of this file is that nothing else in the plugin trusts a hardcoded
// address. Every RVA in signatures.h was read out of build 2.01.00, and the next
// patch will move all of them. Before anything is dereferenced, the vtable at
// that address has to name itself as the class we expect, and if it does not the
// feature refuses to install rather than reading a wild pointer.

namespace gs::rtti
{
    // The decorated name behind a vtable, or nullptr when the address does not
    // carry a plausible complete object locator. Never throws and never reads
    // through a pointer it has not probed first.
    //
    // Example return: ".?AVUIGamePlayControlRootWorldMap@uiCommonScript@pa@@"
    const char* VtableClassName(const void* vtable);

    // True when the vtable names exactly this decorated class.
    bool VtableIs(const void* vtable, const char* decorated);

    // Whether an address is committed, readable and not a guard page. Used
    // before every read above, and by the scanner for its candidates.
    bool Readable(const void* p, size_t bytes);
    bool ReadableCached(const void* p, size_t bytes);
}
