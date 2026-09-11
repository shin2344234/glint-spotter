#pragma once
#include <cstdint>

// The third person camera, held through its own update.
//
// PlayerCameraTPSMode's vtable has sixteen slots. Slot 2 is Update(this,
// float dt). It is taken by a thunk that stores this and jumps, because the
// object is reallocated during play and a scanned pointer goes stale.
//
// What the object holds, from the disassembly of the update's third helper
// (RVA 0x113CA10): a pivot position at +0x30, a rotation quaternion x, y, z,
// w at +0x40, and the distance behind the pivot at +0x50. The helper builds
// a forward vector as (2(xz + wy), 2(yz - wx), 1 - 2(x^2 + y^2)) on one of
// its paths and places the camera at pivot minus forward times distance.
//
// Session twenty-four read that quaternion live: its yaw put the ray on the
// objects the player was aiming at, and its pitch stayed within five degrees
// of level while the player looked down. So the quaternion is the view's yaw
// and maybe not its pitch. The other candidates are read beside it: the
// three angles at +0xC8, +0xCC, +0xD0 that the helper hands to 0x113D3E0,
// the accumulators at +0x364 and +0x368 they are built from, and the unit
// vector at +0x14C. One session with presses at three known pitches names
// the field.

namespace gs::camera
{
    // Take slot 2 on the class's vtable. Refuses when the slot does not hold
    // the update the analysis named, since that means a different layout.
    bool Install(uintptr_t vtable);
    void Remove();

    // The most recent this, or 0 before the first update.
    uintptr_t This();

    struct Pose
    {
        float pivot[3]{};
        float q[4]{};          // x, y, z, w at +0x40
        float dist = 0;        // +0x50
        float fwd[3]{};        // unit, world axes, from the quaternion
        float pitch = 0;       // radians, positive looking up
        float yaw = 0;         // radians, atan2(fwd.x, fwd.z)
        // The other candidates, raw.
        float ang[3]{};        // +0xC8, +0xCC, +0xD0
        float acc[2]{};        // +0x364, +0x368
        float vec[3]{};        // +0x14C
        float dist80 = 0;      // +0x80
        bool valid = false;    // the object was readable
        bool fwdValid = false; // the quaternion was a unit rotation
    };
    Pose Read();

    // One line with every candidate, for the probe cadence.
    void LogSample(uint64_t sample);

    // Log the pose beside the body's facing yaw, at a press.
    void LogAtPress(float facingYaw);
}
