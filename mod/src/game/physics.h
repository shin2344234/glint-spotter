#pragma once
#include <cstdint>

// A ray against the world's collision, through the game's own wrapper.
//
// CrimsonDesert.exe carries a compact function at RVA 0x3926550 that the
// game uses to cast a ray: it takes a start point, a direction and a
// distance, subtracts the physics frame's offset, converts to doubles,
// builds an hknpRayCastQuery and an hknpClosestHitCollector on its stack,
// calls castRay through the physics world facade at RVA 0x6915FB8, and
// hands back the hit distance and the hit normal. Nothing about Havok's
// structures has to be known to use it, which is why it is used.
//
//   bool Cast(void* unused, int layer, bool flag, const float3* start,
//             const float3* dir, float maxDist, float* outDist,
//             float3* outNormal, bool* outFlag)
//
// The layer word is built as (layer < 0x3B ? layer : 0x40000000 | (layer -
// 0x3B)) | flag << 9 and stored beside a 0xFFFF filter. Which layer means
// "terrain and walls" is settled at runtime: the first press tries a few.

namespace gs::physics
{
    // True when the wrapper's code and the facade look as analysed and the
    // physics world exists. Cheap; call before every cast.
    bool Ready(const char** why);

    struct Hit
    {
        float dist = 0;
        float normal[3]{};
        bool flag = false;
        bool hit = false;
    };

    // Cast from `start` along unit `dir` for `maxDist` units, world space
    // (the transform +0x29C frame). Guarded; a fault returns no hit.
    Hit Cast(const float* start, const float* dir, float maxDist, int layer, bool flag);

    // Is there ground standing between the eye and a point?
    //
    // I marked a glint through a mountain. There is no way to ask this
    // engine for terrain height at range: three investigations and two
    // refutation passes went looking and the answer each time was that no CPU
    // side height query exists. What does exist is the collision the game
    // streams in around the player, a box on the order of a hundred and sixty
    // metres across, and inside that box the ground can be measured.
    //
    // So this samples the sight line: at each step it drops a ray straight down
    // from well above the line and asks where the ground is. Ground above the
    // line means the line goes through a hill. A step that finds nothing below
    // means collision is not loaded there and that step knows nothing, which is
    // the common case past the window.
    //
    // It only ever answers Blocked when it measured the ground doing the
    // blocking. Unknown is not Blocked, because a mod that refuses pins on
    // suspicion is worse than one that occasionally marks through a mountain.
    enum class Sight { Clear, Blocked, Unknown };

    // `blockedAt` receives how far along the line the ground got in the way.
    Sight LineOfSight(const float* eye, const float* target, int steps, float* blockedAt,
                      float* blockedHeight = nullptr);

    // Log the facade's vtable, the world pointer and the frame offset.
    void LogState();
}
