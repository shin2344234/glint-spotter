#include "game/clearcheck.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace gs::clearcheck
{
    namespace
    {
        constexpr float kCell = 16.0f;
    }

    int64_t SpotGrid::Cell(float x, float z)
    {
        return (static_cast<int64_t>(std::floor(x / kCell)) << 32) ^
               static_cast<int64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::floor(z / kCell))));
    }

    void SpotGrid::Clear()
    {
        cells_.clear();
        n_ = 0;
    }

    void SpotGrid::Add(float x, float y, float z)
    {
        cells_[Cell(x, z)].push_back({x, y, z});
        ++n_;
    }

    bool SpotGrid::Near(float x, float y, float z, float across, float up) const
    {
        if (!n_) return false;
        const float r2 = across * across;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz)
            {
                auto it = cells_.find(Cell(x + dx * kCell, z + dz * kCell));
                if (it == cells_.end()) continue;
                for (const Spot& s : it->second)
                {
                    const float ex = s.x - x, ez = s.z - z;
                    if (std::fabs(s.y - y) <= up && ex * ex + ez * ez <= r2) return true;
                }
            }
        return false;
    }

    int Decide(const Pin* pins, int n, const Inputs& in, TakenFn taken,
               std::unordered_map<int64_t, bool>& armed, Done* out, int outMax)
    {
        int got = 0;
        for (int i = 0; i < n && got < outMax; ++i)
        {
            const Pin& p = pins[i];
            if (strcmp(p.label, "Glint") == 0)
            {
                if (in.clearTaken && taken && taken(p.x, p.y, p.z))
                {
                    Done& d = out[got++];
                    d.pin = p;
                    _snprintf_s(d.why, sizeof(d.why), _TRUNCATE, "the save has what it marked as taken");
                }
                continue;
            }
            if (in.clearNear <= 0.0f) continue;
            const float dx = p.x - in.px, dz = p.z - in.pz;
            const float dist = std::sqrt(dx * dx + dz * dz);
            bool& a = armed[p.id];
            if (dist > in.clearNear + kArmMetres)
                a = true;
            else if (a && dist <= in.clearNear)
            {
                Done& d = out[got++];
                d.pin = p;
                _snprintf_s(d.why, sizeof(d.why), _TRUNCATE, "you are %.0f metres from it", dist);
                // Gone from the map once the tick takes it off; a new pin with
                // the same key starts unarmed.
                armed.erase(p.id);
            }
        }
        return got;
    }
}
