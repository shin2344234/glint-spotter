#pragma once
#include <cstdint>

// Hex and float dumps of game objects into the log, guarded.
//
// The way an unknown field gets found in this project: dump the object at
// two moments that differ in one thing, diff offline. Every read sits under
// a handler, so a freed object costs a missing dump and nothing else.

namespace gs::dump
{
    // `bytes` at `obj`, eight dwords a line, each as a float when it looks
    // like one and as hex otherwise. Lines are tagged "[tag +off]".
    void Object(const char* tag, uintptr_t obj, size_t bytes);

    // The entity's status (+0x20), gimmick (+0x30), detect (+0x50) and effect
    // (+0x60) components, each named and dumped. `tag` prefixes the lines.
    void EntityComponents(const char* tag, uintptr_t entity, size_t bytesEach);
}
