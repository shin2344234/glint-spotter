#include "game/snapshot.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/actors.h"
#include "game/rtti.h"

namespace
{
    constexpr uintptr_t kOff_Ent_Comps       = 0x68;
    constexpr uintptr_t kComps_SlotsEnd      = 0x80;
    constexpr uintptr_t kOff_Comps_Transform = 0x1A0;
    constexpr uintptr_t kOff_Tf_Pos          = 0xB4;
    constexpr uintptr_t kOff_Tf_ParentEid    = 0xC8;
    constexpr uintptr_t kOff_Tf_ParentPos    = 0xEC;

    constexpr int    kMaxObjects = 20;
    constexpr size_t kObjBytes   = 0x800;

    struct Obj
    {
        uintptr_t addr = 0;
        size_t bytes = 0;
        char name[64]{};
        uint8_t data[kObjBytes];
    };

    // Two generations, swapped on each press. Static: 20 * 2 KB * 2.
    Obj g_cur[kMaxObjects];
    Obj g_prev[kMaxObjects];
    int g_curN = 0, g_prevN = 0;
    uint64_t g_press = 0;

    uintptr_t Deref(uintptr_t at)
    {
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 8)) return 0;
        return *reinterpret_cast<const uintptr_t*>(at);
    }

    const char* NameOf(uintptr_t obj)
    {
        const uintptr_t vt = Deref(obj);
        const char* n = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
        if (!n) return "?";
        return n[0] == '.' ? n + 4 : n;
    }

    bool Copy(uintptr_t addr, uint8_t* out, size_t* bytes)
    {
        __try
        {
            size_t n = kObjBytes;
            while (n >= 0x40 && !gs::rtti::Readable(reinterpret_cast<const void*>(addr), n)) n /= 2;
            if (n < 0x40) return false;
            memcpy(out, reinterpret_cast<const void*>(addr), n);
            *bytes = n;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void Add(uintptr_t addr, const char* name)
    {
        if (!addr || g_curN >= kMaxObjects) return;
        for (int i = 0; i < g_curN; ++i) if (g_cur[i].addr == addr) return;
        Obj& o = g_cur[g_curN];
        if (!Copy(addr, o.data, &o.bytes)) return;
        o.addr = addr;
        strncpy_s(o.name, sizeof(o.name), name, _TRUNCATE);
        ++g_curN;
    }

    void Hex(const Obj& o)
    {
        const auto* q = reinterpret_cast<const uint32_t*>(o.data);
        GS_LOG("[snap] %s at 0x%p, %zu bytes", o.name, reinterpret_cast<void*>(o.addr), o.bytes);
        for (size_t off = 0; off + 32 <= o.bytes; off += 32)
        {
            const size_t i = off / 4;
            GS_LOG("[snap %s +%03zX] %08X %08X %08X %08X %08X %08X %08X %08X", o.name, off,
                   q[i], q[i+1], q[i+2], q[i+3], q[i+4], q[i+5], q[i+6], q[i+7]);
        }
    }

    bool LooksLikeActorId(uint32_t v)
    {
        const uint32_t top = v >> 24;
        return (top == 0xA0 || top == 0xB0 || top == 0xA1 || top == 0xB1) && (v & 0x00FFFFFF) != 0;
    }

    bool PositionOf(uintptr_t actor, float* out)
    {
        __try
        {
            const uintptr_t comps = Deref(actor + kOff_Ent_Comps);
            if (!comps) return false;
            const uintptr_t tf = Deref(comps + kOff_Comps_Transform);
            if (!tf || !gs::rtti::Readable(reinterpret_cast<const void*>(tf), kOff_Tf_ParentPos + 12)) return false;
            float v[3], pw[3];
            memcpy(v, reinterpret_cast<const void*>(tf + kOff_Tf_Pos), 12);
            const uint32_t parent = *reinterpret_cast<const uint32_t*>(tf + kOff_Tf_ParentEid);
            if (parent != 0xFFFFFFFF && parent != 0)
            {
                memcpy(pw, reinterpret_cast<const void*>(tf + kOff_Tf_ParentPos), 12);
                if (std::isfinite(pw[0]) && std::isfinite(pw[1]) && std::isfinite(pw[2]))
                { v[0] += pw[0]; v[1] += pw[1]; v[2] += pw[2]; }
            }
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
            if (std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[2]) > 1.0e6f) return false;
            out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Everything that changed between two copies of one object. Acts on the
    // first new actor id, or the first new position near the player.
    void Diff(const Obj& a, const Obj& b, float px, float py, float pz, gs::snapshot::Found* found)
    {
        const size_t n = a.bytes < b.bytes ? a.bytes : b.bytes;
        const auto* qa = reinterpret_cast<const uint32_t*>(a.data);
        const auto* qb = reinterpret_cast<const uint32_t*>(b.data);
        int lines = 0;
        for (size_t off = 8; off + 4 <= n; off += 4)
        {
            const uint32_t va = qa[off / 4], vb = qb[off / 4];
            if (va == vb) continue;
            if (lines < 48)
                GS_LOG("[diff %s +%03zX] %08X -> %08X", b.name, off, va, vb);
            ++lines;

            // A new actor id: resolve it and read where that actor is.
            if (!found->valid && LooksLikeActorId(vb) && !LooksLikeActorId(va))
            {
                const uintptr_t e = gs::actors::ByEid(vb);
                float v[3];
                if (e && PositionOf(e, v))
                {
                    found->valid = true;
                    found->x = v[0]; found->y = v[1]; found->z = v[2];
                    sprintf_s(found->how, "actor id %08X at %s+0x%zX, %s at (%.1f, %.1f, %.1f)",
                              vb, b.name, off, NameOf(e), v[0], v[1], v[2]);
                }
                else
                {
                    GS_LOG("[diff] id-shaped %08X appeared at %s+0x%zX but the actor manager %s",
                           vb, b.name, off, gs::actors::Ready() ? "has no entity with that id" : "is not located yet");
                }
            }

            // A new position near the player: three finite floats, world-sized,
            // within eighty units. Checked at this dword as x.
            if (!found->valid && off + 12 <= n)
            {
                float f[3];
                memcpy(f, qb + off / 4, 12);
                if (std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]))
                {
                    const float dx = f[0] - px, dy = f[1] - py, dz = f[2] - pz;
                    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (d > 0.5f && d < 80.0f && std::fabs(f[0]) > 1.0f && std::fabs(f[2]) > 1.0f)
                    {
                        found->valid = true;
                        found->x = f[0]; found->y = f[1]; found->z = f[2];
                        sprintf_s(found->how, "position at %s+0x%zX, %.1f units from the player",
                                  b.name, off, d);
                    }
                }
            }
        }
        if (lines > 48) GS_LOG("[diff %s] ... %d more changed dwords", b.name, lines - 48);
    }
}

namespace gs::snapshot
{
    Found PressAndDiff(uintptr_t playerActor, uintptr_t detectTask, float px, float py, float pz)
    {
        Found found;
        ++g_press;

        // Rotate generations.
        memcpy(g_prev, g_cur, sizeof(g_cur));
        g_prevN = g_curN;
        g_curN = 0;

        // The actor, every component in its block, and the detect task.
        Add(playerActor, "actor");
        const uintptr_t comps = Deref(playerActor + kOff_Ent_Comps);
        for (uintptr_t off = 0; comps && off < kComps_SlotsEnd; off += 8)
        {
            const uintptr_t c = Deref(comps + off);
            if (c) Add(c, NameOf(c));
        }
        if (comps) Add(Deref(comps + kOff_Comps_Transform), "transform");
        if (detectTask) Add(detectTask, "FindDetectTargetTask");

        GS_LOG("[snap] press %llu: %d object(s) copied", static_cast<unsigned long long>(g_press), g_curN);
        if (g_press <= 2)
            for (int i = 0; i < g_curN; ++i) Hex(g_cur[i]);

        if (g_prevN == 0)
        {
            GS_LOG("[snap] baseline taken. Now aim the flash at a glint and press again.");
            return found;
        }

        for (int i = 0; i < g_curN; ++i)
        {
            for (int j = 0; j < g_prevN; ++j)
            {
                if (g_prev[j].addr != g_cur[i].addr) continue;
                Diff(g_prev[j], g_cur[i], px, py, pz, &found);
                break;
            }
        }

        if (found.valid) GS_LOG_OK("[snap] target: %s", found.how);
        else GS_LOG("[snap] nothing in the diff looked like a target; the full dumps above are the record");
        return found;
    }
}
