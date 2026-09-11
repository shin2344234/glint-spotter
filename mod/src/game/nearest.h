#pragma once
#include <cstdint>

// The first world object along a ray from the player.
//
// The transform carries the player's facing as a yaw quaternion right before
// its position, and the actor set holds every entity the manager has handed
// over in the last twelve seconds with its world position. Cast a ray from the
// player along the facing, test every entity as a small sphere in a beam that
// widens with distance, and the first hit along the ray is what the crosshair
// is on. A wide spread turns the beam into a cone, which is what the automatic
// marker uses.
//
// The limit: this hits objects, not terrain.

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
        bool gimmick = false;        // an interactable, the kind the glint is drawn on
        char cls[80]{};
    };

    bool ForwardFromQuat(const float* q, float* fx, float* fz);

    // Cast against the actor set. Fills up to n hits ordered by distance along
    // the ray, nearest first. Returns how many.
    int Cast(uintptr_t playerActor,
             float px, float py, float pz, float fx, float fz,
             float maxAlong, float radius, float spread,
             Candidate* out, int n);
}
