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

    void Fill(gs::nearest::Candidate& cand, const gs::actors::Entity& e, float along, float off, float dy)
    {
        cand.entity = e.ptr;
        cand.eid = e.eid;
        cand.x = e.x; cand.y = e.y; cand.z = e.z;
        cand.along = along;
        cand.off = off;
        cand.dy = dy;
        cand.gimmick = e.gimmick;
        cand.glint = e.glint;
        const uintptr_t vt = Deref(e.ptr);
        const char* cn = vt ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt)) : nullptr;
        strncpy_s(cand.cls, sizeof(cand.cls), cn ? (cn[0] == '.' ? cn + 4 : cn) : "?", _TRUNCATE);
    }

    // Insert keeping the array ordered by `key` ascending; drops the largest
    // when full. Returns the new count.
    template <typename Key>
    int Insert(gs::nearest::Candidate* arr, int count, int cap, const gs::nearest::Candidate& c, Key key)
    {
        if (count == cap && key(c) >= key(arr[cap - 1])) return count;
        int pos = count < cap ? count : cap - 1;
        while (pos > 0 && key(arr[pos - 1]) > key(c))
        {
            arr[pos] = arr[pos - 1];
            --pos;
        }
        arr[pos] = c;
        return count < cap ? count + 1 : count;
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
             float maxAlong, float radius, float spread, bool glintOnly,
             Candidate* out, int n, Candidate* miss, int missN)
    {
        if (!out || n <= 0) return 0;
        static gs::actors::Entity set[4096];
        const int total = gs::actors::Snapshot(set, 4096);
        const auto byAlong = [](const Candidate& c) { return c.along; };
        const auto byOff = [](const Candidate& c) { return c.off; };

        int found = 0, missed = 0;
        for (int i = 0; i < total; ++i)
        {
            const gs::actors::Entity& e = set[i];
            if (!e.ptr || e.ptr == playerActor) continue;
            if (glintOnly && !e.glint) continue;

            const float dx = e.x - px, dz = e.z - pz, dy = e.y - py;
            const float along = dx * fx + dz * fz;
            if (along < 0.5f || along > maxAlong) continue;
            const float ox = dx - along * fx, oz = dz - along * fz;
            const float off = std::sqrt(ox * ox + oz * oz);
            const float band = 4.0f + 0.35f * along;
            const bool hit = off <= radius + spread * along && std::fabs(dy) <= band;
            if (!hit && (!miss || missN <= 0 || off > 3.0f * (radius + spread * along))) continue;

            Candidate cand;
            Fill(cand, e, along, off, dy);
            if (hit) found = Insert(out, found, n, cand, byAlong);
            else missed = Insert(miss, missed, missN, cand, byOff);
        }
        return found;
    }

    bool GroundAlong(float ox, float oy, float oz, float fx, float fy, float fz, float maxT,
                     float* gx, float* gy, float* gz, float* t, uint32_t* sampleEid)
    {
        if (fy > -0.02f) return false;
        static gs::actors::Entity set[4096];
        const int total = gs::actors::Snapshot(set, 4096);
        if (total == 0) return false;

        float h = 0;
        bool haveH = false;
        uint32_t hEid = 0;
        for (float tt = 1.0f; tt <= maxT; tt += 0.5f)
        {
            const float px = ox + fx * tt, py = oy + fy * tt, pz = oz + fz * tt;
            // The nearest entity in the ground plane within six units.
            float bestD2 = 36.0f;
            for (int i = 0; i < total; ++i)
            {
                const float dx = set[i].x - px, dz = set[i].z - pz;
                const float d2 = dx * dx + dz * dz;
                if (d2 < bestD2 && std::fabs(set[i].y - py) < 30.0f)
                {
                    bestD2 = d2;
                    h = set[i].y;
                    hEid = set[i].eid;
                    haveH = true;
                }
            }
            if (!haveH) continue;
            if (py <= h + 0.2f)
            {
                *gx = px; *gy = h; *gz = pz; *t = tt;
                *sampleEid = hEid;
                return true;
            }
        }
        return false;
    }

    int Cast3D(uintptr_t playerActor,
               float ox, float oy, float oz, float fx, float fy, float fz,
               float maxAlong, float radius, float spread, bool glintOnly,
               Candidate* out, int n, Candidate* miss, int missN)
    {
        if (!out || n <= 0) return 0;
        static gs::actors::Entity set[4096];
        const int total = gs::actors::Snapshot(set, 4096);
        const auto byAlong = [](const Candidate& c) { return c.along; };
        const auto byOff = [](const Candidate& c) { return c.off; };

        int found = 0, missed = 0;
        for (int i = 0; i < total; ++i)
        {
            const gs::actors::Entity& e = set[i];
            if (!e.ptr || e.ptr == playerActor) continue;
            if (glintOnly && !e.glint) continue;

            const float dx = e.x - ox, dy = e.y - oy, dz = e.z - oz;
            const float along = dx * fx + dy * fy + dz * fz;
            if (along < 0.5f || along > maxAlong) continue;
            const float px = dx - along * fx, py = dy - along * fy, pz = dz - along * fz;
            const float off = std::sqrt(px * px + py * py + pz * pz);
            const bool hit = off <= radius + spread * along;
            if (!hit && (!miss || missN <= 0 || off > 3.0f * (radius + spread * along))) continue;

            Candidate cand;
            Fill(cand, e, along, off, dy);
            if (hit) found = Insert(out, found, n, cand, byAlong);
            else missed = Insert(miss, missed, missN, cand, byOff);
        }
        return found;
    }
}
