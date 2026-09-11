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

    // Look through an object for a vector: a pointer to an array with a
    // count and a capacity beside it. Reports each one it finds and dumps the
    // first two elements at a few plausible strides.
    //
    // Session fifty-nine found pa::LevelGimmickSceneObjectInfoManager alive at
    // 0x42C81E38A00, one candidate, exactly where static analysis said it could
    // not be reached. Its bytes carry the shape: a pointer at +0x28 with
    // 171 and 171 sitting at +0x30 and +0x34. The game's reflection tables say
    // the records hold a _prefabPath and a _worldTransform, so the test for a
    // real one is a readable string pointer and a float3 that lands in the
    // world the player is standing in.
    void Vectors(const char* tag, uintptr_t obj, size_t bytes, float px, float pz);

    // Every pointer-like qword in `bytes` at `obj`, named by RTTI where the
    // target carries a vtable, and every dword that reads as an entity id.
    //
    // For the question the mod still cannot answer: which object is glinting.
    // Session fifty caught the player's special mode component filling three
    // pointer fields the moment the flash fired, +0x98, +0xA8 and +0xE0, the
    // last of them an IRefCounted, and its +0x40 taking the player's own
    // entity id. Something there knows what the flash lit. This says what
    // those fields point at instead of leaving them as truncated hex.
    void Pointers(const char* tag, uintptr_t obj, size_t bytes);
}
