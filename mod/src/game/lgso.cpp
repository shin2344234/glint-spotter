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
    // 17,728 on the map I test in, so there is room to spare without
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
                // The first list in the record that yields placements, looked
                // for out to +0x100. 2944 put sixteen records' only list at
                // +0x90, which the old 0x70 limit never reached: 345
                // placements, mostly dungeons, tunnels and camps. Most records
                // also keep a second copy of their list past +0x70, which is
                // why only the first is taken; taking every one read the whole
                // table twice.
                if (rec < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(rec), 0x100)) continue;
                bool took = false;
                for (uintptr_t off = 0; off + 16 <= 0x100 && n < cap && !took; off += 8)
                {
                    const uintptr_t a2 = *reinterpret_cast<const uintptr_t*>(rec + off);
                    const uint32_t c = *reinterpret_cast<const uint32_t*>(rec + off + 8);
                    const uint32_t cap2 = *reinterpret_cast<const uint32_t*>(rec + off + 12);
                    if (a2 < 0x10000 || (a2 & 7) != 0) continue;
                    if (c == 0 || c > cap2 || cap2 > 100000) continue;
                    const size_t span = static_cast<size_t>(c) * gs::sig::kOff_LgsoData_Stride;
                    if (!gs::rtti::Readable(reinterpret_cast<const void*>(a2), span)) continue;
                    const int before = n;
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
                    took = n > before;
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

    int OnBearing(float px, float pz, float ox, float oy, float oz,
                  float ux, float uz, float slope,
                  float maxPerp, float perpFrac, float maxVert,
                  float minFromPlayer, float maxRange,
                  Place* out, float* dists, int n, bool anyKind)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        int found = 0;
        for (int i = 0; i < g_n; ++i)
        {
            const float dx = g_places[i].x - ox, dz = g_places[i].z - oz;
            // How far along the ray it lies, and how far off it sits.
            const float along = dx * ux + dz * uz;
            if (along < minFromPlayer || along > maxRange) continue;
            // Scaled, not flat. A fixed tolerance is generous up close and
            // impossible at range, and the crosshair's own steadiness works
            // the other way round.
            // The cone widens with range because a crosshair is steady in
            // degrees, not metres, but it stops widening at thirty. Without a
            // distance ceiling a fixed fraction would be a hundred and fifty
            // metres across at five kilometres, which is wide enough to catch
            // something the player cannot see and call it the answer.
            const float perp = std::fabs(dx * uz - dz * ux);
            float allow = along * perpFrac;
            if (allow < maxPerp) allow = maxPerp;
            if (allow > 60.0f) allow = 60.0f;
            if (perp > allow) continue;
            // And the height, which nothing checked until 0.46.0. The sight
            // line is at oy + slope * along when it gets there; a placement
            // far above or below that is not what the crosshair is on, whatever
            // its shadow does. The tolerance grows a little with range because
            // a building's origin sits at its base and a player aims at its
            // middle.
            // Four per cent, not eight. The first log with heights in it
            // shows how well this separates: on one bearing the placement
            // fifteen hundred metres out sat six metres off the sight line's
            // height while its neighbours at seventeen hundred sat a hundred
            // and sixty-six below. Eight per cent kept both, four keeps only
            // the one the crosshair is actually on.
            const float lineY = oy + slope * along;
            float vAllow = along * 0.04f;
            if (vAllow < maxVert) vAllow = maxVert;
            if (std::fabs(g_places[i].y - lineY) > vAllow) continue;
            if (!anyKind && !Worth(g_places[i].name)) continue;
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

    void LogBands(float ox, float oy, float oz, float ux, float uz, float slope,
                  const float* edges, int bandCount)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int b = 0; b < bandCount; ++b)
        {
            const float lo = edges[b], hi = edges[b + 1];
            int best = -1;
            float bestPerp = 0, bestAlong = 0;
            int inBand = 0;
            for (int i = 0; i < g_n; ++i)
            {
                const float dx = g_places[i].x - ox, dz = g_places[i].z - oz;
                const float a = dx * ux + dz * uz;
                if (a < lo || a >= hi) continue;
                ++inBand;
                const float p = std::fabs(dx * uz - dz * ux);
                if (best >= 0 && p >= bestPerp) continue;
                best = i; bestPerp = p; bestAlong = a;
            }
            if (best < 0)
            {
                GS_LOG("[mark]   %5.0f to %5.0f m: nothing in the table at all", lo, hi);
                continue;
            }
            const float lineY = oy + slope * bestAlong;
            GS_LOG("[mark]   %5.0f to %5.0f m: %d placement(s), closest to the line is %.1f m off "
                   "at %.0f m and %+.0f m in height, record %u element %u \"%s\" at (%.1f, %.1f, %.1f)",
                   lo, hi, inBand, bestPerp, bestAlong, g_places[best].y - lineY,
                   g_places[best].record, g_places[best].element,
                   g_places[best].name[0] ? g_places[best].name : "unnamed",
                   g_places[best].x, g_places[best].y, g_places[best].z);
        }
    }

    int NearLine(float ox, float oz, float ux, float uz, float maxRange,
                 Place* out, float* alongs, float* perps, int n)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        int found = 0;
        for (int i = 0; i < g_n; ++i)
        {
            const float dx = g_places[i].x - ox, dz = g_places[i].z - oz;
            const float a = dx * ux + dz * uz;
            if (a < 1.0f || a > maxRange) continue;
            const float p = std::fabs(dx * uz - dz * ux);
            if (found == n && p >= perps[n - 1]) continue;
            int pos = found < n ? found : n - 1;
            while (pos > 0 && perps[pos - 1] > p)
            {
                out[pos] = out[pos - 1];
                alongs[pos] = alongs[pos - 1];
                perps[pos] = perps[pos - 1];
                --pos;
            }
            out[pos] = g_places[i];
            alongs[pos] = a;
            perps[pos] = p;
            if (found < n) ++found;
        }
        return found;
    }

    bool NearestToLine(float ox, float oz, float ux, float uz, float maxRange,
                       Place* out, float* along, float* perp, bool* refused)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        int best = -1;
        float bestPerp = 0, bestAlong = 0;
        for (int i = 0; i < g_n; ++i)
        {
            const float dx = g_places[i].x - ox, dz = g_places[i].z - oz;
            const float a = dx * ux + dz * uz;
            if (a < 1.0f || a > maxRange) continue;
            const float p = std::fabs(dx * uz - dz * ux);
            if (best >= 0 && p >= bestPerp) continue;
            best = i; bestPerp = p; bestAlong = a;
        }
        if (best < 0) return false;
        *out = g_places[best];
        *along = bestAlong;
        *perp = bestPerp;
        *refused = !Worth(g_places[best].name);
        return true;
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

    void LogCatalog(int maxRecords, int maxPerRecord)
    {
        const uintptr_t mgr = Manager();
        if (!mgr) { GS_LOG("[cat] no manager"); return; }
        __try
        {
            const uint32_t count = *reinterpret_cast<const uint32_t*>(mgr + gs::sig::kOff_Lgso_Count);
            const uintptr_t recs = *reinterpret_cast<const uintptr_t*>(mgr + gs::sig::kOff_Lgso_Records);
            if (!count || recs < 0x10000) return;
            const auto* arr = reinterpret_cast<const uintptr_t*>(recs);
            int shownRecords = 0;
            for (uint32_t i = 0; i < count && shownRecords < maxRecords; ++i)
            {
                const uintptr_t rec = arr[i];
                if (rec < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(rec), 0x70)) continue;
                for (uintptr_t off = 0; off + 16 <= 0x70; off += 8)
                {
                    const uintptr_t a2 = *reinterpret_cast<const uintptr_t*>(rec + off);
                    const uint32_t c = *reinterpret_cast<const uint32_t*>(rec + off + 8);
                    const uint32_t cp = *reinterpret_cast<const uint32_t*>(rec + off + 12);
                    if (a2 < 0x10000 || (a2 & 7) != 0) continue;
                    if (c == 0 || c > cp || cp > 100000) continue;
                    if (!gs::rtti::Readable(reinterpret_cast<const void*>(a2), gs::sig::kOff_LgsoData_Stride))
                        continue;
                    // The string table the record's elements share.
                    const uintptr_t el = a2;
                    for (uintptr_t k = 0; k + 8 <= 0x40; k += 8)
                    {
                        const uintptr_t tab = *reinterpret_cast<const uintptr_t*>(el + k);
                        if (tab < 0x10000 || (tab & 7) != 0) continue;
                        if (!gs::rtti::Readable(reinterpret_cast<const void*>(tab), 0x20)) continue;
                        int wrote = 0;
                        for (int e = 0; e < maxPerRecord; ++e)
                        {
                            const uintptr_t d = tab + static_cast<uintptr_t>(e) * 0x20;
                            if (!gs::rtti::Readable(reinterpret_cast<const void*>(d), 16)) break;
                            const uintptr_t cs = *reinterpret_cast<const uintptr_t*>(d);
                            const uint32_t len = *reinterpret_cast<const uint32_t*>(d + 8);
                            if (cs < 0x10000 || len == 0 || len > 200) break;
                            if (!gs::rtti::Readable(reinterpret_cast<const void*>(cs), len)) break;
                            char text[208];
                            uint32_t w = 0;
                            bool ok = true;
                            for (; w < len && w + 1 < sizeof(text); ++w)
                            {
                                const char ch = *reinterpret_cast<const volatile char*>(cs + w);
                                if (ch == 0) break;
                                if (static_cast<unsigned char>(ch) < 0x20 ||
                                    static_cast<unsigned char>(ch) > 0x7E) { ok = false; break; }
                                text[w] = ch;
                            }
                            text[w] = 0;
                            if (!ok || w < 2) break;
                            if (!wrote) GS_LOG("[cat] record %u, list at +%02llX, %u element(s), table 0x%p",
                                               i, static_cast<unsigned long long>(off), c,
                                               reinterpret_cast<void*>(tab));
                            GS_LOG("[cat]   [%3d] %s", e, text);
                            ++wrote;
                        }
                        if (wrote) { ++shownRecords; break; }
                    }
                    if (shownRecords >= maxRecords) break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void LogKinds()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // By record, not by name.
        //
        // Session seventy-three showed why. The counts came out at one to
        // eleven each across two hundred and fifty-six names, against 17,728
        // placements, and the names repeat inside a record: every element of
        // record 5 reads "AbyssIsland_0083_Phase00_00" or a sibling. The
        // string the probe reads is the record's, not the element's, because
        // it takes the first descriptor from a table the whole record shares
        // and nothing yet says which entry an element wants.
        //
        // That makes the record the unit worth knowing. Record 13 is
        // "Challenge_Sealed_Artifact_Her", which is my test glint, and record 5
        // is an abyss island. A list of 171 records with their names and
        // counts is short enough to read and is what a real Kinds default has
        // to be written against.
        struct Rec { char name[56]; int count; };
        static Rec recs[512];
        int rn = 0;
        for (int i = 0; i < g_n; ++i)
        {
            const uint16_t r = g_places[i].record;
            if (r >= 512) continue;
            if (r + 1 > rn) rn = r + 1;
            if (!recs[r].count && g_places[i].name[0])
                strncpy_s(recs[r].name, sizeof(recs[r].name), g_places[i].name, _TRUNCATE);
            ++recs[r].count;
        }
        int worth = 0;
        for (int i = 0; i < g_n; ++i) if (Worth(g_places[i].name)) ++worth;
        GS_LOG("[lgso] %d placement(s) across %d record(s); %d worth a pin under the current Kinds",
               g_n, rn, worth);
        for (int r = 0; r < rn; ++r)
        {
            if (!recs[r].count) continue;
            GS_LOG("[lgso]   record %3d  %5d placement(s)  %-46s %s", r, recs[r].count,
                   recs[r].name[0] ? recs[r].name : "(no name)",
                   Worth(recs[r].name) ? "" : "(refused)");
        }
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
