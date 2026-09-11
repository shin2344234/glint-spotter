#include "game/camera.h"

#include <Windows.h>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/rtti.h"
#include "game/signatures.h"
#include "game/typescan.h"
#include "hook/vtable.h"

// Shared with thunk.asm. The thunk writes gs_cameraThis on every call.
extern "C" void* gs_cameraOriginal = nullptr;
extern "C" void* volatile gs_cameraThis = nullptr;
extern "C" void gs_CameraThunk();

namespace
{
    gs::vtable::Swap g_swap;

    struct Raw
    {
        float pivot[3];
        float q[4];
        float dist;
    };

    bool ReadRaw(uintptr_t obj, Raw* r)
    {
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(obj), 0x60)) return false;
            const auto* b = reinterpret_cast<const uint8_t*>(obj);
            memcpy(r->pivot, b + gs::sig::kOff_Cam_Pivot, 12);
            memcpy(r->q, b + gs::sig::kOff_Cam_Quat, 16);
            memcpy(&r->dist, b + gs::sig::kOff_Cam_Distance, 4);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

namespace gs::camera
{
    bool Install(uintptr_t vtable)
    {
        if (!vtable || g_swap.installed) return false;

        // The slot must hold the update the analysis named, or a pointer
        // outside the image (another mod's hook, which this stacks on). A
        // different in-image function means the layout moved.
        uintptr_t base = 0;
        size_t size = 0;
        gs::typescan::ModuleRange(base, size);
        const auto* slots = reinterpret_cast<void* const*>(vtable);
        if (!gs::rtti::Readable(slots, (sig::kSlotCameraUpdate + 1) * sizeof(void*))) return false;
        const uintptr_t held = reinterpret_cast<uintptr_t>(slots[sig::kSlotCameraUpdate]);
        const uintptr_t expect = base + sig::kCameraTPSUpdate;
        const bool inImage = held >= base && held < base + size;
        if (inImage && held != expect)
        {
            GS_LOG_ERR("[camera] slot %d holds 0x%p, expected 0x%p; the layout differs from the analysed exe, not hooking",
                       sig::kSlotCameraUpdate, reinterpret_cast<void*>(held), reinterpret_cast<void*>(expect));
            return false;
        }

        if (!vtable::Install(vtable, sig::kSlotCameraUpdate, reinterpret_cast<void*>(&gs_CameraThunk), g_swap))
        {
            GS_LOG_ERR("[camera] could not take slot %d on the TPS camera vtable", sig::kSlotCameraUpdate);
            return false;
        }
        gs_cameraOriginal = g_swap.original;
        GS_LOG_OK("[camera] slot %d on vtable 0x%p was 0x%p (%s), now the capture thunk",
                  sig::kSlotCameraUpdate, reinterpret_cast<void*>(vtable), g_swap.original,
                  held == expect ? "the update at RVA 0x113C100" : "someone else's hook, stacked on");
        return true;
    }

    void Remove()
    {
        if (!g_swap.installed) return;
        bool leftAlone = false;
        if (vtable::Restore(g_swap, leftAlone)) GS_LOG("[camera] slot restored");
        else if (leftAlone) GS_LOG("[camera] slot now holds someone else's hook, left in place");
    }

    uintptr_t This() { return reinterpret_cast<uintptr_t>(gs_cameraThis); }

    Pose Read()
    {
        Pose p;
        const uintptr_t obj = This();
        Raw r{};
        if (!obj || !ReadRaw(obj, &r)) return p;
        memcpy(p.pivot, r.pivot, sizeof(p.pivot));
        memcpy(p.q, r.q, sizeof(p.q));
        p.dist = r.dist;
        p.valid = true;

        const float x = r.q[0], y = r.q[1], z = r.q[2], w = r.q[3];
        const float n2 = x * x + y * y + z * z + w * w;
        if (!std::isfinite(n2) || n2 < 0.9f || n2 > 1.1f) return p;
        // The helper's own formula, RVA 0x113CDF8 onward.
        float fx = 2.0f * (x * z + w * y);
        float fy = 2.0f * (y * z - w * x);
        float fz = 1.0f - 2.0f * (x * x + y * y);
        const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (len < 0.5f) return p;
        fx /= len; fy /= len; fz /= len;
        p.fwd[0] = fx; p.fwd[1] = fy; p.fwd[2] = fz;
        p.pitch = std::asin(fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy));
        p.yaw = std::atan2(fx, fz);
        p.fwdValid = true;
        return p;
    }

    void LogAtPress(float facingYaw)
    {
        const Pose p = Read();
        if (!p.valid)
        {
            GS_LOG("[camera] no camera object yet (this 0x%p)", gs_cameraThis);
            return;
        }
        GS_LOG("[camera] this 0x%p pivot (%.2f, %.2f, %.2f) quat (%.4f, %.4f, %.4f, %.4f) distance %.2f",
               gs_cameraThis, p.pivot[0], p.pivot[1], p.pivot[2], p.q[0], p.q[1], p.q[2], p.q[3], p.dist);
        if (!p.fwdValid)
        {
            GS_LOG("[camera] the quaternion is not a unit rotation; no view ray from it");
            return;
        }
        float dyaw = p.yaw - facingYaw;
        while (dyaw > 3.14159f) dyaw -= 6.28318f;
        while (dyaw < -3.14159f) dyaw += 6.28318f;
        GS_LOG("[camera] forward (%.3f, %.3f, %.3f): pitch %.1f deg, yaw %.1f deg; body facing %.1f deg, camera minus body %.1f deg",
               p.fwd[0], p.fwd[1], p.fwd[2], p.pitch * 57.2958f, p.yaw * 57.2958f, facingYaw * 57.2958f, dyaw * 57.2958f);
    }
}
