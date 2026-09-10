#pragma once
#include <cstdint>

// The first world object along a ray from the player.
//
// The game's own aim field on the character control component is real but
// conditional: session seventeen had it 10.7 units ahead while the flash was
// aimed, session eighteen had it zeroed at the same kind of press. This is the
// route that needs nothing conditional. The transform carries the player's
// facing as a yaw quaternion right before its position, the actor manager
// carries every entity, and each entity's transform carries a world position.
// Cast a ray from the player along the facing, test every entity in range
// against it as a small sphere, and the first hit along the ray is what the
// crosshair is on.
//
// The limit: this ray hits objects, not terrain. Marking bare ground with
// nothing on it needs the engine's own raycast, which is a later step.

namespace gs::nearest
{
    struct Candidate
    {
        uintptr_t entity = 0;
        uint32_t eid = 0;
        float x = 0, y = 0, z = 0;   // world
        float along = 0;             // distance along the ray to the closest point
        float off = 0;               // distance from the ray at that point, XZ
        float dy = 0;                // height difference from the player
        char cls[80]{};
    };

    // Facing from a yaw quaternion (x, y, z, w) about the vertical axis, as a
    // unit vector in the XZ plane. Returns false if it is not a yaw rotation.
    bool ForwardFromQuat(const float* q, float* fx, float* fz);

    // Walk the actor manager's list and test every entity against the ray.
    // The beam is `radius` wide at the player and widens by `spread` per unit
    // of distance, so a far object needs to be closer to the line than a near
    // one is allowed to be. Fills up to n hits ordered by distance along the
    // ray, nearest first. Returns how many.
    int Cast(uintptr_t manager, uintptr_t playerActor,
             float px, float py, float pz, float fx, float fz,
             float maxAlong, float radius, float spread,
             Candidate* out, int n);
}
