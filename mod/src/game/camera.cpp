#include "game/camera.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/rtti.h"
#include "game/signatures.h"
#include "game/typescan.h"
#include "hook/vtable.h"

// Shared with thunk.asm. The thunk writes gs_cameraThis on every call.
extern "C" void* gs_cameraOriginal = nullptr;
extern "C" void* volatile gs_cameraThis = nullptr;
extern "C" volatile uint64_t gs_cameraCalls = 0;
extern "C" void gs_CameraThunk();

namespace
{
    gs::vtable::Swap g_swap;

    struct Raw
    {
        float pivot[3];
        float q[4];
        float dist;
        float ang[3];
        float acc[2];
        float vec[3];
        float dist80;
        float world1[3];
        float world2[3];
    };

    bool ReadRaw(uintptr_t obj, Raw* r)
    {
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(obj), 0x380)) return false;
            const auto* b = reinterpret_cast<const uint8_t*>(obj);
            memcpy(r->pivot, b + gs::sig::kOff_Cam_Pivot, 12);
            memcpy(r->q, b + gs::sig::kOff_Cam_Quat, 16);
            memcpy(&r->dist, b + gs::sig::kOff_Cam_Distance, 4);
            memcpy(r->ang, b + 0xC8, 12);
            memcpy(&r->acc[0], b + 0x364, 4);
            memcpy(&r->acc[1], b + 0x368, 4);
            memcpy(r->vec, b + 0x14C, 12);
            memcpy(&r->dist80, b + 0x80, 4);
            memcpy(r->world1, b + 0x94, 12);
            memcpy(r->world2, b + 0xB4, 12);
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
        // The update is known by its first bytes, which read the fade weight
        // off this, and not by its address: the 11 September patch moved
        // every function and the address check refused a correct slot. The
        // field's own displacement is left out of the compare. The 17
        // September patch moved it from +0x338 to +0x340 and nothing else in
        // those bytes, and the vtable has already been named by RTTI, so the
        // shape is the check and the offset is not.
        bool looksLikeUpdate = inImage &&
            gs::rtti::Readable(reinterpret_cast<const void*>(held), sizeof(sig::kCameraUpdatePrologue));
        for (size_t i = 0; looksLikeUpdate && i < sizeof(sig::kCameraUpdatePrologue); ++i)
        {
            if (i >= sig::kCameraUpdateFieldAt && i < sig::kCameraUpdateFieldAt + 3) continue;
            if (reinterpret_cast<const uint8_t*>(held)[i] != sig::kCameraUpdatePrologue[i]) looksLikeUpdate = false;
        }
        if (inImage && !looksLikeUpdate)
        {
            GS_LOG_ERR("[camera] slot %d holds 0x%p and it does not start like the update (recorded at 0x%p); not hooking",
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
                  looksLikeUpdate ? (held == expect ? "the update, at the recorded address" : "the update, moved since the record")
                                  : "someone else's hook, stacked on");
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
    uint64_t Calls() { return gs_cameraCalls; }

    Pose Read()
    {
        Pose p;
        const uintptr_t obj = This();
        Raw r{};
        if (!obj || !ReadRaw(obj, &r)) return p;
        memcpy(p.pivot, r.pivot, sizeof(p.pivot));
        memcpy(p.q, r.q, sizeof(p.q));
        memcpy(p.ang, r.ang, sizeof(p.ang));
        memcpy(p.acc, r.acc, sizeof(p.acc));
        memcpy(p.vec, r.vec, sizeof(p.vec));
        p.dist = r.dist;
        p.dist80 = r.dist80;
        // The world position, believed only when its two copies agree.
        float w1[3], w2[3];
        memcpy(w1, r.world1, 12);
        memcpy(w2, r.world2, 12);
        if (std::isfinite(w1[0]) && std::isfinite(w1[1]) && std::isfinite(w1[2]) &&
            std::fabs(w1[0] - w2[0]) < 1.0f && std::fabs(w1[1] - w2[1]) < 1.0f &&
            std::fabs(w1[2] - w2[2]) < 1.0f &&
            std::fabs(w1[0]) < 60000.0f && std::fabs(w1[2]) < 60000.0f &&
            std::fabs(w1[0]) + std::fabs(w1[2]) > 1.0f)
        {
            p.world[0] = w1[0]; p.world[1] = w1[1]; p.world[2] = w1[2];
            p.worldValid = true;
        }

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

    void LogSample(uint64_t sample)
    {
        const Pose p = Read();
        if (!p.valid) return;
        // Every candidate on one line, so a press at a known pitch can be
        // matched against each of them.
        GS_LOG("[cam %llu] quat pitch %.1f yaw %.1f | angles +C8 %.3f +CC %.3f +D0 %.3f | acc +364 %.3f +368 %.3f | vec +14C (%.3f, %.3f, %.3f) | dist +50 %.2f +80 %.2f",
               static_cast<unsigned long long>(sample),
               p.fwdValid ? p.pitch * 57.2958f : 0.0f, p.fwdValid ? p.yaw * 57.2958f : 0.0f,
               p.ang[0], p.ang[1], p.ang[2], p.acc[0], p.acc[1], p.vec[0], p.vec[1], p.vec[2], p.dist, p.dist80);
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
        LogSample(0);
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
        // What the other two candidates would say the view is, if they are
        // angles in radians or a direction the camera sits along.
        GS_LOG("[camera] if +C8/+CC are yaw/pitch in radians: yaw %.1f pitch %.1f deg; in degrees: yaw %.1f pitch %.1f",
               p.ang[0] * 57.2958f, p.ang[1] * 57.2958f, p.ang[0], p.ang[1]);
        const float vl = std::sqrt(p.vec[0] * p.vec[0] + p.vec[1] * p.vec[1] + p.vec[2] * p.vec[2]);
        if (vl > 0.5f)
            GS_LOG("[camera] if +14C points from the pivot to the camera: the view back along it has pitch %.1f yaw %.1f deg",
                   -std::asin(p.vec[1] / vl) * 57.2958f, std::atan2(-p.vec[0], -p.vec[2]) * 57.2958f);
        DumpAtPress();
    }

    // The whole mode object and the whole owner, as floats, at a press.
    // Session twenty-five's quaternion pitch stayed within thirteen degrees
    // of level while the player looked down at a glint, and the offset at
    // +0xC8 turned out to be the shoulder offset. Three presses at three
    // known pitches, dumped whole, name the pitch field offline.
    void DumpOne(const char* tag, uintptr_t obj, size_t bytes)
    {
        __try
        {
            size_t n = bytes;
            while (n >= 0x40 && !gs::rtti::Readable(reinterpret_cast<const void*>(obj), n)) n /= 2;
            if (n < 0x40) return;
            const auto* f = reinterpret_cast<const float*>(obj);
            const auto* u = reinterpret_cast<const uint32_t*>(obj);
            char line[400];
            for (size_t off = 0; off + 32 <= n; off += 32)
            {
                int w = 0;
                for (int k = 0; k < 8; ++k)
                {
                    const size_t i = off / 4 + k;
                    const float v = f[i];
                    int r;
                    if (std::isfinite(v) && (v == 0.0f || (std::fabs(v) > 1e-4f && std::fabs(v) < 1e6f)))
                        r = _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " %11.4f", v);
                    else
                        r = _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, "  0x%08X ", u[i]);
                    if (r < 0) break;
                    w += r;
                }
                line[w] = 0;
                GS_LOG("[%s +%03zX]%s", tag, off, line);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void DumpAtPress()
    {
        const uintptr_t obj = This();
        if (!obj) return;
        DumpOne("camdump", obj, 0x400);
        uintptr_t owner = 0;
        __try
        {
            if (gs::rtti::Readable(reinterpret_cast<const void*>(obj + 8), 8)) owner = *reinterpret_cast<const uintptr_t*>(obj + 8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            owner = 0;
        }
        if (owner)
        {
            const uintptr_t vt = gs::rtti::Readable(reinterpret_cast<const void*>(owner), 8) ? *reinterpret_cast<const uintptr_t*>(owner) : 0;
            const char* n = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
            GS_LOG("[camera] owner at +8 is 0x%p %s", reinterpret_cast<void*>(owner), n ? n : "?");
            DumpOne("owndump", owner, 0x400);
        }
    }
}
