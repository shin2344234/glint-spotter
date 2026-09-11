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

    // The FindDetectTargetTask hanging off the detect component, or 0.
    uintptr_t DetectTask();

    // The two components themselves, for the probe that dumps them.
    uintptr_t DetectComponent();
    uintptr_t SpecialComponent();

    // Every actor the detect component, the special mode component and the
    // FindDetectTargetTask are holding a pointer to, each with the offset it
    // sat at, its entity id, all three of its candidate positions, its
    // distance and how far off the given bearing it is. The component's own
    // target scalars go with it.
    //
    // Resolve() takes the first actor it finds and session nineteen's log
    // shows that landing on a real target, but first is not a criterion. This
    // lists them all so the offset that holds the true target can be named
    // from evidence rather than picked.
    void DescribeTargets(float px, float py, float pz, float ox, float oz, float ux, float uz);

    // The aim point the character control component keeps at +0x318, in the
    // sub-level's local space. Session seventeen: it moved when the flash was
    // aimed at a glint and sat 10.7 units from the player. Valid only when it
    // is finite and within reach.
    bool AimPointLocal(uintptr_t charctl, float lx, float ly, float lz, float* out);

    // The detect component's distance to its current target, at +0x3EC.
    // Session nineteen: FLT_MAX at every press with no target; session
    // seventeen: 0.5 while aimed at a glint, with +0x580 at 3.0. Returns
    // false when there is no target.
    bool DetectDistance(float* out);
}
