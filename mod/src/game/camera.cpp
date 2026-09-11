#include "game/camera.h"

#include <Windows.h>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/rtti.h"
#include "hook/vtable.h"

// Shared with thunk.asm. The thunk writes gs_cameraThis on every call.
extern "C" void* gs_cameraOriginal = nullptr;
extern "C" void* volatile gs_cameraThis = nullptr;
extern "C" void gs_CameraThunk();

namespace
{
    constexpr int kSlotUpdate = 19;
    gs::vtable::Swap g_swap;

    bool ReadFloats(uintptr_t obj, gs::camera::Fields* f)
    {
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(obj), 0x370)) return false;
            const auto* q = reinterpret_cast<const uint8_t*>(obj);
            memcpy(&f->c0, q + 0xC0, 4);   memcpy(&f->c4, q + 0xC4, 4);
            memcpy(&f->c8, q + 0xC8, 4);   memcpy(&f->cc, q + 0xCC, 4);
            memcpy(&f->d0, q + 0xD0, 4);   memcpy(&f->f104, q + 0x104, 4);
            memcpy(&f->f1dc, q + 0x1DC, 4); memcpy(&f->f1e0, q + 0x1E0, 4);
            memcpy(&f->f360, q + 0x360, 4);
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
        if (!vtable::Install(vtable, kSlotUpdate, reinterpret_cast<void*>(&gs_CameraThunk), g_swap))
        {
            GS_LOG_ERR("[camera] could not take slot %d on the TPS camera vtable", kSlotUpdate);
            return false;
        }
        gs_cameraOriginal = g_swap.original;
        GS_LOG_OK("[camera] slot %d on vtable 0x%p was 0x%p, now the capture thunk",
                  kSlotUpdate, reinterpret_cast<void*>(vtable), g_swap.original);
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

    Fields Read()
    {
        Fields f;
        const uintptr_t obj = This();
        if (obj && ReadFloats(obj, &f)) f.valid = true;
        return f;
    }

    void LogAtPress(float facingYaw, float px, float py, float pz, float fx, float fz)
    {
        const Fields f = Read();
        if (!f.valid)
        {
            GS_LOG("[camera] no camera object yet (this 0x%p)", gs_cameraThis);
            return;
        }
        GS_LOG("[camera] this 0x%p; facing yaw %.3f rad (%.1f deg)", gs_cameraThis, facingYaw, facingYaw * 57.2958f);
        GS_LOG("[camera] +C0 %.4f +C4 %.4f +C8 %.4f +CC %.4f +D0 %.4f +104 %.4f +1DC %.4f +1E0 %.4f +360 %.4f",
               f.c0, f.c4, f.c8, f.cc, f.d0, f.f104, f.f1dc, f.f1e0, f.f360);

        // Every field that could be a pitch, as the ground point it would give:
        // a ray from 1.6 units above the feet, tilted down by that angle, meets
        // the feet's plane at 1.6 / tan(pitch) along the facing. Both signs.
        struct C { const char* name; float v; };
        const C cands[] = {{"+C0", f.c0}, {"+C4", f.c4}, {"+C8", f.c8}, {"+CC", f.cc}, {"+D0", f.d0},
                           {"+104", f.f104}, {"+1DC", f.f1dc}, {"+1E0", f.f1e0}, {"+360", f.f360}};
        for (const C& c : cands)
        {
            if (!std::isfinite(c.v) || std::fabs(c.v) < 0.02f || std::fabs(c.v) > 1.5f) continue;
            const float t = std::tan(std::fabs(c.v));
            if (t < 0.02f) continue;
            const float along = 1.6f / t;
            if (along > 200.0f) continue;
            GS_LOG("[camera]   if %s (%.3f rad, %.1f deg) is the pitch, the ground point is %.1f along at (%.1f, %.1f, %.1f)",
                   c.name, c.v, c.v * 57.2958f, along, px + fx * along, py, pz + fz * along);
        }
    }
}
