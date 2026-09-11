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

    // Just the gimmick component at +0x30, and the sub-object it keeps at
    // +0x438. Used on several nodes at once, with the flash on and again
    // with it off: the field that means "this one is glinting" is the one
    // that moves for the revealed node and for no other.
    void GimmickState(const char* tag, uintptr_t entity, size_t bytes);

    // How busy an entity's effect component is: how many of the pointers in
    // its first `bytes` are set. A revealed object has the reveal effect
    // attached to it, so this should rise when the flash lights it and fall
    // when the flash ends, and not move on its neighbours.
    int EffectActivity(uintptr_t entity, size_t bytes);
}
