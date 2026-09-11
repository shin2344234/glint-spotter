#include "game/lgso.h"

#include <Windows.h>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "core/settings.h"
#include "game/rtti.h"
#include "game/signatures.h"
#include "game/typescan.h"

namespace
{
    // 17,728 on the map Seth tests in, so there is room to spare without
    // making the read allocate.
    constexpr int kMax = 40000;

    std::mutex g_mutex;
    gs::lgso::Place g_places[kMax];
    int g_n = 0;
    uintptr_t g_loadedFrom = 0;

    uintptr_t Deref(uintptr_t at)
    {
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 8)) return 0;
        return *reinterpret_cast<const uintptr_t*>(at);
    }

    // The manager, straight from the global its own vtable maintains.
    uintptr_t Manager()
    {
        uintptr_t base = 0;
        size_t size = 0;
        if (!gs::typescan::ModuleRange(base, size)) return 0;
        const uintptr_t mgr = Deref(base + gs::sig::kLgsoManagerGlobal);
        if (mgr < 0x10000) return 0;
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(mgr), 0x80)) return 0;
        return mgr;
    }

    int ReadInto(uintptr_t mgr, gs::lgso::Place* out, int cap)
    {
        int n = 0;
        __try
        {
            const uint32_t count = *reinterpret_cast<const uint32_t*>(mgr + gs::sig::kOff_Lgso_Count);
            const uintptr_t recs = *reinterpret_cast<const uintptr_t*>(mgr + gs::sig::kOff_Lgso_Records);
            if (!count || count > 100000 || recs < 0x10000) return 0;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(recs), 8ull * count)) return 0;
            const auto* arr = reinterpret_cast<const uintptr_t*>(recs);
            for (uint32_t i = 0; i < count && n < cap; ++i)
            {
                const uintptr_t rec = arr[i];
                if (rec < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(rec), 0x70)) continue;
                for (uintptr_t off = 0; off + 16 <= 0x70 && n < cap; off += 8)
                {
                    const uintptr_t a2 = *reinterpret_cast<const uintptr_t*>(rec + off);
                    const uint32_t c = *reinterpret_cast<const uint32_t*>(rec + off + 8);
                    const uint32_t cap2 = *reinterpret_cast<const uint32_t*>(rec + off + 12);
                    if (a2 < 0x10000 || (a2 & 7) != 0) continue;
                    if (c == 0 || c > cap2 || cap2 > 100000) continue;
                    const size_t span = static_cast<size_t>(c) * gs::sig::kOff_LgsoData_Stride;
                    if (!gs::rtti::Readable(reinterpret_cast<const void*>(a2), span)) continue;
                    for (uint32_t e = 0; e < c && n < cap; ++e)
                    {
                        const uintptr_t el = a2 + static_cast<uintptr_t>(e) * gs::sig::kOff_LgsoData_Stride;
                        const float* q = reinterpret_cast<const float*>(el + gs::sig::kOff_LgsoData_Transform);
                        // The rotation is the check on the whole read. A wrong
                        // stride lands mid-record and the four floats stop
                        // summing to one, which is how a bad layout announces
                        // itself instead of producing plausible rubbish.
                        const float unit = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
                        if (!(unit > 0.98f && unit < 1.02f)) continue;
                        const float* p = q + 4;
                        if (!(p[0] == p[0]) || !(p[1] == p[1]) || !(p[2] == p[2])) continue;
                        if (std::fabs(p[0]) > 1.0e6f || std::fabs(p[2]) > 1.0e6f) continue;
                        gs::lgso::Place& pl = out[n++];
                        pl.x = p[0]; pl.y = p[1]; pl.z = p[2];
                        pl.record = static_cast<uint16_t>(i);
                        pl.element = static_cast<uint16_t>(e);
                        pl.name[0] = 0;
                        // The name. A pointer near the head of an element
                        // leads to string descriptors, each a character
                        // pointer, a length and a hash, repeating every 0x20
                        // bytes. Session sixty-six read
                        // "Mission_PororinVillage_Bell_All_Calphade" and
                        // "Hernand_Bell" out of record 0 that way.
                        for (uintptr_t k = 0; k + 8 <= 0x40 && !pl.name[0]; k += 8)
                        {
                            const uintptr_t pv = *reinterpret_cast<const uintptr_t*>(el + k);
                            if (pv < 0x10000 || (pv & 7) != 0) continue;
                            if (!gs::rtti::Readable(reinterpret_cast<const void*>(pv), 16)) continue;
                            const uintptr_t cs = *reinterpret_cast<const uintptr_t*>(pv);
                            const uint32_t len = *reinterpret_cast<const uint32_t*>(pv + 8);
                            if (cs < 0x10000 || len == 0 || len > 200) continue;
                            if (!gs::rtti::Readable(reinterpret_cast<const void*>(cs), len)) continue;
                            size_t w = 0;
                            bool ok = true;
                            for (; w < len && w + 1 < sizeof(pl.name); ++w)
                            {
                                const char ch = *reinterpret_cast<const volatile char*>(cs + w);
                                if (ch == 0) break;
                                if (static_cast<unsigned char>(ch) < 0x20 ||
                                    static_cast<unsigned char>(ch) > 0x7E) { ok = false; break; }
                                pl.name[w] = ch;
                            }
                            pl.name[w] = 0;
                            if (!ok || w < 2) pl.name[0] = 0;
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return n;
    }
}

namespace gs::lgso
{
    int Load()
    {
        const uintptr_t mgr = Manager();
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!mgr)
        {
            if (g_loadedFrom) GS_LOG("[lgso] the manager is gone; the table is dropped");
            g_loadedFrom = 0;
            g_n = 0;
            return 0;
        }
        if (mgr == g_loadedFrom && g_n > 0) return g_n;
        g_n = ReadInto(mgr, g_places, kMax);
        g_loadedFrom = mgr;
        GS_LOG_OK("[lgso] %d placement(s) read from the level gimmick table at 0x%p", g_n,
                  reinterpret_cast<void*>(mgr));
        return g_n;
    }

    int Count()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_n;
    }

    int OnBearing(float px, float pz, float ox, float oz, float ux, float uz,
                  float maxPerp, float minFromPlayer, float maxRange,
                  Place* out, float* dists, int n)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        int found = 0;
        for (int i = 0; i < g_n; ++i)
        {
            const float dx = g_places[i].x - ox, dz = g_places[i].z - oz;
            // How far along the ray it lies, and how far off it sits.
            const float along = dx * ux + dz * uz;
            if (along < minFromPlayer || along > maxRange) continue;
            const float perp = std::fabs(dx * uz - dz * ux);
            if (perp > maxPerp) continue;
            if (!Worth(g_places[i].name)) continue;
            const float fx = g_places[i].x - px, fz = g_places[i].z - pz;
            const float fromPlayer = std::sqrt(fx * fx + fz * fz);
            if (fromPlayer < minFromPlayer) continue;
            int pos = found;
            while (pos > 0 && dists[pos - 1] > along)
            {
                if (pos < n) { out[pos] = out[pos - 1]; dists[pos] = dists[pos - 1]; }
                --pos;
            }
            if (pos < n) { out[pos] = g_places[i]; dists[pos] = along; }
            if (found < n) ++found;
        }
        return found;
    }

    bool Worth(const char* name)
    {
        if (!name || !name[0]) return false;
        if (_strnicmp(name, "sector_", 7) == 0) return false;
        const char* list = gs::Settings::Get().kinds;
        if (!list[0]) return true;
        char lower[64];
        size_t i = 0;
        for (; name[i] && i + 1 < sizeof(lower); ++i)
            lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(name[i])));
        lower[i] = 0;
        char want[64];
        const char* p = list;
        while (*p)
        {
            while (*p == ' ' || *p == ',') ++p;
            size_t w = 0;
            while (*p && *p != ',' && w + 1 < sizeof(want))
                want[w++] = static_cast<char>(tolower(static_cast<unsigned char>(*p++)));
            while (*p && *p != ',') ++p;
            want[w] = 0;
            if (w && strstr(lower, want)) return true;
        }
        return false;
    }

    void LogKinds()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Distinct names, counted. Held in a small fixed table because this
        // runs on the worker thread and should not allocate.
        struct Kind { char name[56]; int count; uint16_t record; };
        static Kind kinds[256];
        int kn = 0;
        int unnamed = 0;
        for (int i = 0; i < g_n; ++i)
        {
            if (!g_places[i].name[0]) { ++unnamed; continue; }
            int k = 0;
            for (; k < kn; ++k) if (strcmp(kinds[k].name, g_places[i].name) == 0) break;
            if (k == kn)
            {
                if (kn >= 256) continue;
                strncpy_s(kinds[kn].name, sizeof(kinds[kn].name), g_places[i].name, _TRUNCATE);
                kinds[kn].count = 0;
                kinds[kn].record = g_places[i].record;
                ++kn;
            }
            ++kinds[k].count;
        }
        int worth = 0;
        for (int i = 0; i < g_n; ++i) if (Worth(g_places[i].name)) ++worth;
        GS_LOG("[lgso] %d distinct name(s) across %d placement(s), %d with no name at all; "
               "%d placement(s) are worth a pin", kn, g_n, unnamed, worth);
        for (int k = 0; k < kn; ++k)
            GS_LOG("[lgso]   %5d  record %3u  %-46s %s", kinds[k].count, kinds[k].record,
                   kinds[k].name, Worth(kinds[k].name) ? "" : "(refused)");
    }

    int Near(float px, float pz, Place* out, int n)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        int found = 0;
        for (int i = 0; i < g_n; ++i)
        {
            const float dx = g_places[i].x - px, dz = g_places[i].z - pz;
            const float d = std::sqrt(dx * dx + dz * dz);
            int pos = found;
            while (pos > 0)
            {
                const float ax = out[pos - 1].x - px, az = out[pos - 1].z - pz;
                if (std::sqrt(ax * ax + az * az) <= d) break;
                if (pos < n) out[pos] = out[pos - 1];
                --pos;
            }
            if (pos < n) out[pos] = g_places[i];
            if (found < n) ++found;
        }
        return found;
    }
}
