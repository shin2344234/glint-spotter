#include "game/nearest.h"

#include <Windows.h>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/rtti.h"

namespace
{
    constexpr uintptr_t kOff_Mgr_List        = 0x190;
    constexpr uintptr_t kOff_Ent_Eid         = 0x60;
    constexpr uintptr_t kOff_Ent_Comps       = 0x68;
    constexpr uintptr_t kOff_Comps_Transform = 0x1A0;
    constexpr uintptr_t kOff_Tf_WorldPos     = 0x29C;
    constexpr uint32_t  kMaxWalk             = 40000;

    uintptr_t Deref(uintptr_t at)
    {
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 8)) return 0;
        return *reinterpret_cast<const uintptr_t*>(at);
    }

    bool WorldPos(uintptr_t e, float* out)
    {
        __try
        {
            const uintptr_t comps = Deref(e + kOff_Ent_Comps);
            if (!comps) return false;
            const uintptr_t tf = Deref(comps + kOff_Comps_Transform);
            if (!tf || !gs::rtti::Readable(reinterpret_cast<const void*>(tf + kOff_Tf_WorldPos), 12)) return false;
            memcpy(out, reinterpret_cast<const void*>(tf + kOff_Tf_WorldPos), 12);
            if (!std::isfinite(out[0]) || !std::isfinite(out[1]) || !std::isfinite(out[2])) return false;
            if (std::fabs(out[0]) + std::fabs(out[1]) + std::fabs(out[2]) > 1.0e6f) return false;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ListOf(uintptr_t mgr, uintptr_t* data, uint32_t* count)
    {
        __try
        {
            const uintptr_t list = mgr + kOff_Mgr_List;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(list), 16)) return false;
            *count = *reinterpret_cast<const uint32_t*>(list);
            *data = *reinterpret_cast<const uintptr_t*>(list + 8);
            if (!*data || *count == 0 || *count > kMaxWalk) return false;
            return gs::rtti::Readable(reinterpret_cast<const void*>(*data),
                                      static_cast<size_t>(*count) * sizeof(uintptr_t));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uintptr_t EntryAt(uintptr_t data, uint32_t i)
    {
        __try { return reinterpret_cast<const uintptr_t*>(data)[i]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    uint32_t EidOf(uintptr_t e)
    {
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(e + kOff_Ent_Eid), 4)) return 0;
            return *reinterpret_cast<const uint32_t*>(e + kOff_Ent_Eid);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
}

namespace gs::nearest
{
    bool ForwardFromQuat(const float* q, float* fx, float* fz)
    {
        // A rotation about the vertical axis has x and z near zero. The angle is
        // twice atan2(y, w), and forward is +Z turned by it.
        if (std::fabs(q[0]) > 0.2f || std::fabs(q[2]) > 0.2f) return false;
        const float n = std::sqrt(q[1] * q[1] + q[3] * q[3]);
        if (n < 0.5f) return false;
        const float yaw = 2.0f * std::atan2(q[1] / n, q[3] / n);
        *fx = std::sin(yaw);
        *fz = std::cos(yaw);
        return true;
    }

    int Cast(uintptr_t manager, uintptr_t playerActor,
             float px, float py, float pz, float fx, float fz,
             float maxAlong, float radius, float spread,
             Candidate* out, int n)
    {
        if (!manager || !out || n <= 0) return 0;
        uintptr_t data = 0;
        uint32_t count = 0;
        if (!ListOf(manager, &data, &count)) return 0;

        int found = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uintptr_t e = EntryAt(data, i);
            if (!e || e == playerActor) continue;
            float w[3];
            if (!WorldPos(e, w)) continue;

            // Project the entity onto the ray in the XZ plane. `along` is where on
            // the ray its closest point is; `off` is how far it sits from the ray.
            const float dx = w[0] - px, dz = w[2] - pz, dy = w[1] - py;
            const float along = dx * fx + dz * fz;
            if (along < 0.5f || along > maxAlong) continue;
            const float ox = dx - along * fx, oz = dz - along * fz;
            const float off = std::sqrt(ox * ox + oz * oz);
            if (off > radius + spread * along) continue;
            if (std::fabs(dy) > 20.0f) continue;

            Candidate cand;
            cand.entity = e;
            cand.eid = EidOf(e);
            cand.x = w[0]; cand.y = w[1]; cand.z = w[2];
            cand.along = along;
            cand.off = off;
            cand.dy = dy;
            const uintptr_t vt = Deref(e);
            const char* cn = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
            strncpy_s(cand.cls, sizeof(cand.cls), cn ? (cn[0] == '.' ? cn + 4 : cn) : "?", _TRUNCATE);

            // Nearest along the ray first. Insert sorted, drop the farthest when full.
            if (found == n && along >= out[n - 1].along) continue;
            int pos = found < n ? found : n - 1;
            while (pos > 0 && out[pos - 1].along > along)
            {
                out[pos] = out[pos - 1];
                --pos;
            }
            out[pos] = cand;
            if (found < n) ++found;
        }
        return found;
    }
}
