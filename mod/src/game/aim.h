#pragma once
#include <cstdint>

// What the flash is aimed at, read from the game's own detect component.
//
// Session thirteen mapped the played body's component block, and one of the
// components in it is ClientDetectActorComponent: the blinding flash's own
// per-actor state. The game resolves what the flash is focused on, and it has
// to keep that somewhere in there. Rather than guess the offset, this walks the
// component's pointer-sized fields when asked, names each one through RTTI,
// and takes the first that is an actor other than the player. That actor's
// position comes from the same transform walk the player's does.
//
// Nothing here needs the camera. If the game is pointing at a glint, this reads
// the glint.

namespace gs::aim
{
    struct Target
    {
        float x = 0, y = 0, z = 0;
        char cls[96]{};        // decorated class of the actor found
        uintptr_t actor = 0;
        uintptr_t foundAt = 0; // offset in the component where the pointer sat
        bool valid = false;
    };

    // The player's actor, its detect component and its special mode component,
    // resolved from the special component the scan finds.
    void SetPlayerActor(uintptr_t actor);
    void SetDetectComponent(uintptr_t comp);
    void SetSpecialComponent(uintptr_t comp);

    // True while the flash is on: the special component's +0x40 holds the
    // player's actor id, and zero otherwise.
    bool FlashActive();

    // Search both components for an actor pointer that is not the player and
    // read its position. Logs every candidate it considers on the first few
    // calls so the layout becomes known.
    Target Resolve();
}
