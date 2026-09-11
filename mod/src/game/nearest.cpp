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
        cand.lit = e.lit;
        cand.pickup = e.pickup;
        cand.knowledge = e.knowledge;
        cand.locked = e.locked;
        cand.how = e.how;
        memcpy(cand.name, e.name, sizeof(cand.name));
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
            // "only": only what the flash reveals. A pickup, or a gimmick
            // that carries knowledge, which is what an object that yields
            // something after a puzzle looks like before the puzzle is done.
            if (glintOnly && !(e.pickup || (e.gimmick && e.knowledge))) continue;

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

    int CastBearing(uintptr_t playerActor,
                    float ox, float oy, float oz, float fx, float fz,
                    float maxAlong, float maxAngle, float band, bool markedOnly,
                    Candidate* out, int n)
    {
        if (!out || n <= 0) return 0;
        static gs::actors::Entity set[4096];
        const int total = gs::actors::Snapshot(set, 4096);
        const float flen = std::sqrt(fx * fx + fz * fz);
        if (flen < 1e-3f) return 0;
        const float ux = fx / flen, uz = fz / flen;
        const auto byOff = [](const Candidate& c) { return c.off; };   // the bearing, in radians

        int found = 0;
        for (int i = 0; i < total; ++i)
        {
            const gs::actors::Entity& e = set[i];
            if (!e.ptr || e.ptr == playerActor) continue;
            if (markedOnly && !(e.pickup || (e.gimmick && e.knowledge))) continue;

            const float dx = e.x - ox, dz = e.z - oz, dy = e.y - oy;
            const float flat = std::sqrt(dx * dx + dz * dz);
            if (flat < 0.5f || flat > maxAlong) continue;
            if (std::fabs(dy) > band) continue;
            // The angle between the two bearings, from their dot and cross.
            const float dot = (dx * ux + dz * uz) / flat;
            const float cross = (dx * uz - dz * ux) / flat;
            const float angle = std::fabs(std::atan2(cross, dot));
            if (angle > maxAngle) continue;

            Candidate cand;
            Fill(cand, e, flat, angle, dy);
            found = Insert(out, found, n, cand, byOff);
        }
        return found;
    }

    void Reach(float px, float py, float pz, int* bands, float* farthest)
    {
        static gs::actors::Entity set[4096];
        const int total = gs::actors::Snapshot(set, 4096);
        for (int i = 0; i < 5; ++i) bands[i] = 0;
        *farthest = 0;
        for (int i = 0; i < total; ++i)
        {
            const float dx = set[i].x - px, dy = set[i].y - py, dz = set[i].z - pz;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > *farthest) *farthest = d;
            ++bands[d < 30 ? 0 : d < 60 ? 1 : d < 120 ? 2 : d < 300 ? 3 : 4];
        }
    }

    int Closest(uintptr_t playerActor, float px, float py, float pz, Candidate* out, int n)
    {
        if (!out || n <= 0) return 0;
        static gs::actors::Entity set[4096];
        const int total = gs::actors::Snapshot(set, 4096);
        const auto byAlong = [](const Candidate& c) { return c.along; };
        int found = 0;
        for (int i = 0; i < total; ++i)
        {
            const gs::actors::Entity& e = set[i];
            if (!e.ptr || e.ptr == playerActor) continue;
            const float dx = e.x - px, dy = e.y - py, dz = e.z - pz;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (found == n && d >= out[n - 1].along) continue;
            Candidate cand;
            Fill(cand, e, d, 0.0f, dy);
            found = Insert(out, found, n, cand, byAlong);
        }
        return found;
    }

    // The terrain height under a point, estimated from the entities around
    // it: the four nearest within forty units in the ground plane, weighted
    // by the inverse square of their distance. Session twenty-eight's walk
    // wanted a sample within six units of the path and found none for two
    // hundred units, then took the first thing below the ray. Objects far
    // above the ray are not the ground under it and are left out.
    bool TerrainHeight(const gs::actors::Entity* set, int total, float px, float py, float pz,
                       float* h, int* used, uint32_t* eid)
    {
        struct S { float d2; float y; uint32_t eid; };
        S best[4];
        int n = 0;
        for (int i = 0; i < total; ++i)
        {
            const float dx = set[i].x - px, dz = set[i].z - pz;
            const float d2 = dx * dx + dz * dz;
            if (d2 > 1600.0f) continue;
            if (set[i].y > py + 12.0f || py - set[i].y > 80.0f) continue;
            if (n == 4 && d2 >= best[3].d2) continue;
            int pos = n < 4 ? n : 3;
            while (pos > 0 && best[pos - 1].d2 > d2) { best[pos] = best[pos - 1]; --pos; }
            best[pos] = {d2, set[i].y, set[i].eid};
            if (n < 4) ++n;
        }
        if (n == 0) return false;
        float wsum = 0, hsum = 0;
        for (int i = 0; i < n; ++i)
        {
            const float w = 1.0f / (best[i].d2 + 1.0f);
            wsum += w;
            hsum += w * best[i].y;
        }
        *h = hsum / wsum;
        *used = n;
        *eid = best[0].eid;
        return true;
    }

    bool GroundAlong(float ox, float oy, float oz, float fx, float fy, float fz, float maxT,
                     float* gx, float* gy, float* gz, float* t, uint32_t* sampleEid)
    {
        if (fy > -0.02f) return false;
        static gs::actors::Entity set[4096];
        const int total = gs::actors::Snapshot(set, 4096);
        if (total == 0) return false;

        float nextLog = 25.0f;
        for (float tt = 1.0f; tt <= maxT; tt += 0.5f)
        {
            const float px = ox + fx * tt, py = oy + fy * tt, pz = oz + fz * tt;
            float h = 0;
            int used = 0;
            uint32_t eid = 0;
            if (!TerrainHeight(set, total, px, py, pz, &h, &used, &eid)) continue;
            if (tt >= nextLog)
            {
                nextLog += 25.0f;
                GS_LOG("[mark]     terrain profile: %.0f out, ray at %.1f, ground about %.1f from %d sample(s)", tt, py, h, used);
            }
            if (py <= h + 0.3f)
            {
                *gx = px; *gy = h; *gz = pz; *t = tt;
                *sampleEid = eid;
                return true;
            }
        }
        return false;
    }

    int Cast3D(uintptr_t playerActor,
               float ox, float oy, float oz, float fx, float fy, float fz,
               float maxAlong, float radius, float spread, bool glintOnly,
               Candidate* out, int n, Candidate* miss, int missN, float nearAll)
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
            // "only": only what the flash reveals. A pickup, or a gimmick
            // that carries knowledge, which is what an object that yields
            // something after a puzzle looks like before the puzzle is done.
            if (glintOnly && !(e.pickup || (e.gimmick && e.knowledge))) continue;

            const float dx = e.x - ox, dy = e.y - oy, dz = e.z - oz;
            const float along = dx * fx + dy * fy + dz * fz;
            const float range = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool withinReach = nearAll > 0.0f && range <= nearAll;
            if (!withinReach && (along < 0.5f || along > maxAlong)) continue;
            const float px = dx - along * fx, py = dy - along * fy, pz = dz - along * fz;
            const float off = std::sqrt(px * px + py * py + pz * pz);
            const bool hit = withinReach || off <= radius + spread * along;
            if (!hit && (!miss || missN <= 0 || off > 3.0f * (radius + spread * along))) continue;

            Candidate cand;
            Fill(cand, e, along, off, dy);
            if (hit) found = Insert(out, found, n, cand, byAlong);
            else missed = Insert(miss, missed, missN, cand, byOff);
        }
        return found;
    }
}
