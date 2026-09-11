#pragma once
#include <cstdint>

// The first world object along a ray from the player.
//
// The actor set holds every entity the manager has handed over in the last
// twelve seconds with its world position. Two casts:
//
// Cast: a level ray in the ground plane from the given origin along (fx, fz),
// with a vertical band that widens with distance, because the body's facing
// has no pitch. Session twenty-one's real hits sat six to eleven units below
// the player at twenty units out, and a 3D beam around a level ray rejected
// all of them (session twenty-three found nothing all session).
//
// Cast3D: a true beam around a unit direction in three axes, for when the
// camera's forward vector is in hand. Then a floor below is outside the beam
// because the ray is actually pointed where the player looks.
//
// Both hit objects, not terrain. Both can also report the nearest misses, so
// a session log shows what a tighter or wider beam would have taken.

namespace gs::nearest
{
    struct Candidate
    {
        uintptr_t entity = 0;
        uint32_t eid = 0;
        float x = 0, y = 0, z = 0;   // world
        float along = 0;             // distance along the ray to the closest point
        float off = 0;               // distance from the ray at that point
        float dy = 0;                // height relative to the ray origin
        bool gimmick = false;        // carries a ClientGimmickActorComponent
        bool glint = false;          // that component's detect mode target byte is set
        bool lit = false;            // the reveal effect is on it right now
        bool pickup = false;         // a gimmick that yields an item
        bool knowledge = false;      // carries a knowledge component
        bool locked = false;
        bool parented = false;
        char name[48]{};             // the gimmick's node name
        char cls[80]{};
    };

    bool ForwardFromQuat(const float* q, float* fx, float* fz);

    // Level ray. Accepts when the XZ distance from the ray is within
    // radius + spread * along and |dy| within 4 + 0.35 * along. Hits are
    // ordered by distance along the ray. `miss` receives up to missN rejected
    // candidates with the smallest XZ offset, for the log.
    int Cast(uintptr_t playerActor,
             float px, float py, float pz, float fx, float fz,
             float maxAlong, float radius, float spread, bool glintOnly,
             Candidate* out, int n, Candidate* miss = nullptr, int missN = 0);

    // Beam around a unit direction. Accepts when the 3D distance from the ray
    // is within radius + spread * along.
    int Cast3D(uintptr_t playerActor,
               float ox, float oy, float oz, float fx, float fy, float fz,
               float maxAlong, float radius, float spread, bool glintOnly,
               Candidate* out, int n, Candidate* miss = nullptr, int missN = 0);

    // How far the set reaches from a point: counts within 30, 60, 120, 300
    // units and beyond, and the farthest entity. Session twenty-seven's
    // glint was more than 90 units out and the casts stopped at 60.
    void Reach(float px, float py, float pz, int* bands, float* farthest);

    // The n entities closest to a point, any direction, nearest first. `along`
    // carries the straight-line distance and `off` is zero.
    int Closest(uintptr_t playerActor, float px, float py, float pz, Candidate* out, int n);

    // Where a ray meets the ground, with the ground estimated from the
    // entities near its path: objects stand on the terrain, so the height of
    // the nearest one within a few units is the terrain there. Walks the ray
    // half a unit at a time until it drops below that estimate. Session
    // twenty-four put a pin 33 units out from a plane at the player's feet
    // where the terrain fell away much sooner. Returns false when the ray
    // never reaches the ground within maxT or nothing stands near its path.
    bool GroundAlong(float ox, float oy, float oz, float fx, float fy, float fz, float maxT,
                     float* gx, float* gy, float* gz, float* t, uint32_t* sampleEid);
}
