#pragma once
#include <cstdint>

// A per-frame moment on the game's thread, while the player is playing.
//
// Slot 170 gave the mod a way to place a pin, but only from inside a call the
// game makes at map open and pin placement. The feature needs a tick of its own
// choosing: flash held, glint seen, pin dropped, with no map open. The minimap
// root control's update at slot 35 runs every frame the minimap is drawn, which
// is all of play, and it runs on the UI thread that also owns icon creation.
//
// Another mod holds that slot already. This stacks: the slot's current value is
// read, kept, and tail-jumped to from an assembly thunk that forwards every
// argument register untouched. Nothing is assumed about the signature or about
// whose function was there.

namespace gs::tick
{
    // Install on the minimap root vtable. Returns false if the slot could not be
    // taken. Safe to call once only.
    bool Install(uintptr_t miniVtable);
    void Remove();

    uint64_t Count();
    uint32_t ThreadId();     // of the tick, once seen, else 0

    // The diff probe: sample the root control's bytes on the tick, and log the
    // float fields that changed since the previous sample. Off by default; the
    // worker turns it on for discovery sessions.
    void SetProbe(bool on);

    // A second probe target found by the scan, such as the player's special
    // mode component, diffed on the same cadence to find the flash flag.
    void SetExtraProbe(const char* label, void* object, size_t bytes);

    // Ask the tick to place a pin on its next run, on the game's thread.
    // Position is the best one known; the caller says where it came from.
    void RequestMark(float x, float z, const char* label);
    void SetWorldRoot(void* root);
}
