#pragma once
#include <cstdint>

// The third person camera, held through its own update.
//
// PlayerCameraTPSMode keeps the view as scalars on its object: slot 19, its
// per-frame update, stores to this+0xC0, +0xC8, +0xCC, +0xD0, +0x104, +0x1DC,
// +0x1E0 and +0x360 after clamp and interpolate helpers. Yaw, pitch and the
// distance behind the player are among them. The object is reallocated during
// play, so a pointer found by scanning goes stale; a thunk on slot 19 takes
// the address from every call instead and never calls anything.
//
// Which field is the pitch is settled from the log: at each chord press every
// candidate is printed beside the yaw the transform already gives, and the
// ground point each candidate would produce.

namespace gs::camera
{
    // Take slot 19 on the class's vtable. Safe to call once.
    bool Install(uintptr_t vtable);
    void Remove();

    // The most recent this, or 0 before the first update.
    uintptr_t This();

    struct Fields
    {
        float c0 = 0, c4 = 0, c8 = 0, cc = 0, d0 = 0, f104 = 0, f1dc = 0, f1e0 = 0, f360 = 0;
        bool valid = false;
    };
    Fields Read();

    // Log the fields beside the facing yaw, and for each angle-shaped one the
    // point where a ray from eye height at that pitch meets the ground at the
    // player's feet, along the facing.
    void LogAtPress(float facingYaw, float px, float py, float pz, float fx, float fz);
}
