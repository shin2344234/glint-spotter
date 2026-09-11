#pragma once
#include <cstdint>

// The third person camera, held through its own update.
//
// PlayerCameraTPSMode's vtable has sixteen slots. Slot 2 is Update(this,
// float dt). It is taken by a thunk that stores this and jumps, because the
// object is reallocated during play and a scanned pointer goes stale.
//
// What the object holds, from the disassembly of the update's third helper
// (RVA 0x113CA10): a pivot position at +0x30, the camera's rotation as a
// quaternion x, y, z, w at +0x40, and the distance behind the pivot at +0x50.
// The helper builds the forward vector as (2(xz + wy), 2(yz - wx),
// 1 - 2(x^2 + y^2)), normalises it, and puts the camera at pivot minus
// forward times distance. The same forward is the view ray this mod wants.

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
        float q[4]{};          // x, y, z, w
        float dist = 0;
        float fwd[3]{};        // unit, world axes
        float pitch = 0;       // radians, positive looking up
        float yaw = 0;         // radians, atan2(fwd.x, fwd.z), same convention as the body facing
        bool valid = false;    // the object was readable
        bool fwdValid = false; // the quaternion was a unit rotation
    };
    Pose Read();

    // Log the pose beside the body's facing yaw, at a press.
    void LogAtPress(float facingYaw);
}
