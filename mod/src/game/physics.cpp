#include "game/physics.h"

#include <Windows.h>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/rtti.h"
#include "game/typescan.h"

namespace
{
    constexpr uintptr_t kWrapperRva     = 0x03926550;
    constexpr uintptr_t kFacadeRva      = 0x06915FB8;   // the static physics world facade
    constexpr uintptr_t kWorldPtrRva    = 0x06C16B20;   // hknpWorld*, null until a world exists
    constexpr uintptr_t kFrameOffsetRva = 0x06C16B80;   // float4 subtracted from every position
    constexpr int       kSlotCastRay    = 61;

    // The wrapper's first twelve bytes: mov rax,rsp; mov [rax+8],rbx; mov
    // [rax+0x10],rsi; mov [rax+0x18],rdi starts.
    const uint8_t kPrologue[12] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48};

    using CastFn = bool (*)(void* unused, int layer, bool flag, const float* start, const float* dir,
                            float maxDist, float* outDist, float* outNormal, bool* outFlag);

    uintptr_t g_base = 0;
    size_t g_size = 0;

    bool Base()
    {
        if (g_base) return true;
        return gs::typescan::ModuleRange(g_base, g_size);
    }

    bool InImage(uintptr_t p) { return p >= g_base && p < g_base + g_size; }

    bool ReadQ(uintptr_t at, uintptr_t* out)
    {
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 8)) return false;
            *out = *reinterpret_cast<const uintptr_t*>(at);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallGuarded(CastFn fn, void* facade, int layer, bool flag, const float* start, const float* dir,
                     float maxDist, float* outDist, float* outNormal, bool* outFlag, bool* result)
    {
        __try
        {
            *result = fn(facade, layer, flag, start, dir, maxDist, outDist, outNormal, outFlag);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

namespace gs::physics
{
    bool Ready(const char** why)
    {
        if (!Base()) { *why = "module range unknown"; return false; }
        const uintptr_t fn = g_base + kWrapperRva;
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(fn), sizeof(kPrologue)) ||
            memcmp(reinterpret_cast<const void*>(fn), kPrologue, sizeof(kPrologue)) != 0)
        {
            *why = "the wrapper at RVA 0x3926550 does not start with the analysed bytes";
            return false;
        }
        uintptr_t vt = 0, slot = 0, world = 0;
        if (!ReadQ(g_base + kFacadeRva, &vt) || !InImage(vt)) { *why = "the facade's vtable is not in the image"; return false; }
        if (!ReadQ(vt + kSlotCastRay * 8, &slot) || !InImage(slot)) { *why = "the facade's castRay slot is not in the image"; return false; }
        if (!ReadQ(g_base + kWorldPtrRva, &world) || !world) { *why = "no physics world yet"; return false; }
        return true;
    }

    Hit Cast(const float* start, const float* dir, float maxDist, int layer, bool flag)
    {
        Hit h;
        const char* why = nullptr;
        if (!Ready(&why)) return h;
        float s[4] = {start[0], start[1], start[2], 0};
        float d[4] = {dir[0], dir[1], dir[2], 0};
        float dist = 0;
        float normal[4] = {0, 0, 0, 0};
        bool f = false;
        bool result = false;
        const CastFn fn = reinterpret_cast<CastFn>(g_base + kWrapperRva);
        if (!CallGuarded(fn, reinterpret_cast<void*>(g_base + kFacadeRva), layer, flag, s, d, maxDist,
                         &dist, normal, &f, &result))
        {
            GS_LOG_ERR("[physics] the cast faulted (layer %d); no hit", layer);
            return h;
        }
        if (!result || !std::isfinite(dist) || dist < 0.0f) return h;
        h.hit = true;
        h.dist = dist;
        h.normal[0] = normal[0]; h.normal[1] = normal[1]; h.normal[2] = normal[2];
        h.flag = f;
        return h;
    }

    void LogState()
    {
        const char* why = nullptr;
        const bool ok = Ready(&why);
        uintptr_t vt = 0, world = 0;
        ReadQ(g_base + kFacadeRva, &vt);
        ReadQ(g_base + kWorldPtrRva, &world);
        float off[4] = {0, 0, 0, 0};
        if (gs::rtti::Readable(reinterpret_cast<const void*>(g_base + kFrameOffsetRva), 16))
            memcpy(off, reinterpret_cast<const void*>(g_base + kFrameOffsetRva), 16);
        const char* wn = world && gs::rtti::Readable(reinterpret_cast<const void*>(world), 8)
                             ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(*reinterpret_cast<const uintptr_t*>(world)))
                             : nullptr;
        GS_LOG("[physics] %s; facade vtable 0x%p, world 0x%p (%s), frame offset (%.1f, %.1f, %.1f, %.1f)",
               ok ? "ready" : why, reinterpret_cast<void*>(vt), reinterpret_cast<void*>(world), wn ? wn : "?",
               off[0], off[1], off[2], off[3]);
    }
}
