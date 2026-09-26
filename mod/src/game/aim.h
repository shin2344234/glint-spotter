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
    // resolved from the special component the scan finds. The special one is
    // remembered together with its vtable and its owner, and dropped the
    // moment either changes, because a load frees it and the block is reused.
    void SetPlayerActor(uintptr_t actor);
    void SetDetectComponent(uintptr_t comp);
    void SetSpecialComponent(uintptr_t comp);

    // True while the flash is on: the special component's +0x40 holds the
    // player's actor id, and zero otherwise. False when the component is
    // gone, or when it belongs to a body that is no longer the player's.
    bool FlashActive();
    // The two special mode records the flag sits in: each is 0x18 bytes at
    // +0x30 and +0x48 of the component and starts with a 16-bit mode id,
    // 0xFFFF when empty. False when the component cannot be read.
    bool ModeRecords(uint16_t* first, uint16_t* second, uint32_t* firstTail, uint32_t* secondTail);
    // The name gamedata/specialmode.staticinfo gives a mode's row, or null.
    const char* ModeName(uint16_t row);
    // The row count the game's own special mode table reports. The mode check
    // in FlashActive runs only while it matches the table aim.cpp was written
    // from; zero, the default, leaves every mode counting as the flash.
    void SetModeTableRows(uint32_t rows);

    // Search both components for an actor pointer that is not the player and
    // read its position. Logs every candidate it considers on the first few
    // calls so the layout becomes known.
    Target Resolve();

    // The FindDetectTargetTask hanging off the detect component, or 0.
    uintptr_t DetectTask();

    // The two components themselves, for the probe that dumps them.
    uintptr_t DetectComponent();
    uintptr_t SpecialComponent();

    // One actor the detect system is holding, resolved into the world.
    struct Held
    {
        float x = 0, y = 0, z = 0;     // world
        float dist = 0;                // flat, from the player
        float angle = 0;               // degrees off the crosshair
        uint32_t eid = 0;
        uintptr_t at = 0;              // the offset it sat at
        char where[16]{};              // which object held it
        char cls[64]{};
        bool inPools = false;          // the manager holds it too, so it is live
        bool valid = false;
    };

    // Where the player stands, in both frames, so a target's position can be
    // resolved: the sub-level origin is the world minus the local.
    struct Eye
    {
        float px = 0, py = 0, pz = 0;      // player, world
        float lx = 0, ly = 0, lz = 0;      // player, local
        float ox = 0, oz = 0;              // the eye the bearing starts from
        float ux = 0, uz = 0;              // unit view bearing
    };

    // Every actor the detect component, the special mode component and the
    // FindDetectTargetTask are holding a pointer to, each with the offset it
    // sat at, its entity id, its position in whichever frame lands it near the
    // player, its distance and how far off the crosshair it is. Returns the
    // one nearest the crosshair.
    //
    // Session nineteen is why this is worth another look. Build 0.6.0 dropped
    // this route reading its result as a fixed reference "104 units off", and
    // that reading does not survive the arithmetic: the actor sat at local
    // (-835.600, 536.055, -299.897) with the player at local (-731.963,
    // 562.228, -301.813), which is 103.7 metres west of him, and the pin went
    // to world (-9835.601, 0, -4299.897), which is that same spot correctly
    // converted. The pointer held still across eight presses in thirty-four
    // seconds, and so did the player, to three decimal places. Session
    // forty-seven then measured a real glint at a hundred and eighteen metres.
    // A target a hundred metres out is what this feature is looking for.
    Held DescribeTargets(const Eye& eye, bool log);

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
