#include "game/nearest.h"

#include <Windows.h>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/actors.h"
#include "game/rtti.h"

namespace
{
    uintptr_t Deref(uintptr_t at)
    {
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 8)) return 0;
        return *reinterpret_cast<const uintptr_t*>(at);
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

    int Cast(uintptr_t playerActor,
             float px, float py, float pz, float fx, float fz,
             float maxAlong, float radius, float spread,
             Candidate* out, int n)
    {
        if (!out || n <= 0) return 0;
        static gs::actors::Entity set[4096];
        const int total = gs::actors::Snapshot(set, 4096);

        int found = 0;
        for (int i = 0; i < total; ++i)
        {
            const gs::actors::Entity& e = set[i];
            if (!e.ptr || e.ptr == playerActor) continue;

            const float dx = e.x - px, dz = e.z - pz, dy = e.y - py;
            const float along = dx * fx + dz * fz;
            if (along < 0.5f || along > maxAlong) continue;
            const float ox = dx - along * fx, oz = dz - along * fz;
            const float off = std::sqrt(ox * ox + oz * oz);
            if (off > radius + spread * along) continue;
            if (std::fabs(dy) > 20.0f) continue;

            Candidate cand;
            cand.entity = e.ptr;
            cand.eid = e.eid;
            cand.x = e.x; cand.y = e.y; cand.z = e.z;
            cand.along = along;
            cand.off = off;
            cand.dy = dy;
            const uintptr_t vt = Deref(e.ptr);
            const char* cn = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
            strncpy_s(cand.cls, sizeof(cand.cls), cn ? (cn[0] == '.' ? cn + 4 : cn) : "?", _TRUNCATE);

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
