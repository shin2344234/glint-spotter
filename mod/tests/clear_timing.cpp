// The pin check that takes pins off when they are done, at the sizes 2.05.00
// gives it: 45,604 placements in the level gimmick table, and 256 pins, the
// most one save keeps in the file (and 1024, the most the map list holds).
//
// The decision now runs on the key thread over a grid of taken places. This
// drives the real clearcheck.cpp, checks it gives the answers the old loop
// gave, and times both. The old loop is copied below as it stood in a7e59ab
// (savemap::TakenNear, a scan of every taken place per pin), because the code
// it lived in reads the game and cannot be loaded here.
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/clearcheck.cpp"

namespace
{
    int failures = 0;
    constexpr int kPlaces = 45604;
    constexpr float kJoinMetres = 3.0f, kJoinHeight = 6.0f;

    struct Place { float x, y, z; };
    std::vector<Place> g_places;
    std::vector<Place> g_takenList;     // the old layout
    gs::clearcheck::SpotGrid g_takenGrid;

    uint32_t g_seed = 12345;
    float Rand(float lo, float hi)
    {
        g_seed = g_seed * 1664525u + 1013904223u;
        return lo + (hi - lo) * static_cast<float>(g_seed >> 8) / static_cast<float>(1u << 24);
    }

    // a7e59ab's savemap::TakenNear, without the lock.
    bool OldTakenNear(float x, float y, float z)
    {
        for (const Place& s : g_takenList)
        {
            const float ex = s.x - x, ez = s.z - z;
            if (std::fabs(s.y - y) <= kJoinHeight && ex * ex + ez * ez <= kJoinMetres * kJoinMetres) return true;
        }
        return false;
    }
    bool NewTakenNear(float x, float y, float z) { return g_takenGrid.Near(x, y, z, kJoinMetres, kJoinHeight); }

    void Check(const char* what, bool ok)
    {
        if (ok) { printf("ok    %s\n", what); return; }
        ++failures;
        printf("FAIL  %s\n", what);
    }

    double Ms(LARGE_INTEGER a, LARGE_INTEGER b)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
    }

    // Takes `taken` of the placements as taken, both layouts.
    void Take(int taken)
    {
        g_takenList.clear();
        g_takenGrid.Clear();
        for (int i = 0; i < taken; ++i)
        {
            const Place& p = g_places[(static_cast<size_t>(i) * 7919u) % g_places.size()];
            g_takenList.push_back(p);
            g_takenGrid.Add(p.x, p.y, p.z);
        }
    }

    // Glint pins on placements, half of them on taken ones when there are any.
    std::vector<gs::clearcheck::Pin> Pins(int n)
    {
        std::vector<gs::clearcheck::Pin> pins(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            const Place& p = (i % 2 == 0 && !g_takenList.empty())
                                 ? g_takenList[(static_cast<size_t>(i) * 31u) % g_takenList.size()]
                                 : g_places[(static_cast<size_t>(i) * 104729u + 17u) % g_places.size()];
            pins[i].id = 1000 + i;
            pins[i].x = p.x + 0.4f;   // a marker lands a little off the placement
            pins[i].y = p.y;
            pins[i].z = p.z - 0.3f;
            strcpy_s(pins[i].label, "Glint");
        }
        return pins;
    }

    // One pass the old way: every pin against every taken place, the way
    // AutoClear did it once a second on the game's thread.
    int OldPass(const std::vector<gs::clearcheck::Pin>& pins)
    {
        int got = 0;
        for (const auto& p : pins)
            if (OldTakenNear(p.x, p.y, p.z)) ++got;
        return got;
    }

    int NewPass(const std::vector<gs::clearcheck::Pin>& pins, std::vector<gs::clearcheck::Done>& out)
    {
        static std::unordered_map<int64_t, bool> armed;
        gs::clearcheck::Inputs in;
        in.clearTaken = true;
        in.clearNear = 10.0f;
        in.px = 0.0f;
        in.pz = 0.0f;
        out.resize(pins.size() + 1);
        return gs::clearcheck::Decide(pins.data(), static_cast<int>(pins.size()), in, NewTakenNear, armed,
                                      out.data(), static_cast<int>(out.size()));
    }

    // Best of a number of passes, so a context switch does not count.
    template <typename F> double Best(int runs, F f)
    {
        double best = 1e9;
        for (int r = 0; r < runs; ++r)
        {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            f();
            QueryPerformanceCounter(&b);
            best = std::min(best, Ms(a, b));
        }
        return best;
    }
}

int main()
{
    // Pywel is about 20 km across; the table's placements spread over it,
    // with a few hundred metres of height.
    g_places.reserve(kPlaces);
    for (int i = 0; i < kPlaces; ++i) g_places.push_back({Rand(-16000, 4000), Rand(0, 1500), Rand(-9000, 9000)});

    printf("-- the grid gives the old scan's answers --\n");
    {
        Take(5000);
        int disagree = 0, yes = 0;
        for (int i = 0; i < 40000; ++i)
        {
            float x, y, z;
            if (i % 2 == 0)
            {
                // Around a taken place, either side of three metres across
                // and six up or down, and across cell edges.
                const Place& p = g_takenList[static_cast<size_t>(i) % g_takenList.size()];
                const float a = Rand(0, 6.2832f), r = Rand(2.5f, 3.5f);
                x = p.x + r * std::cos(a);
                z = p.z + r * std::sin(a);
                y = p.y + Rand(-7, 7);
            }
            else
            {
                x = std::floor(Rand(-16000, 4000) / 16.0f) * 16.0f + Rand(-1, 1);
                y = Rand(0, 1500);
                z = std::floor(Rand(-9000, 9000) / 16.0f) * 16.0f + Rand(-1, 1);
            }
            const bool o = OldTakenNear(x, y, z), n = NewTakenNear(x, y, z);
            if (o != n) ++disagree;
            if (o) ++yes;
        }
        printf("      40000 queries, %d near a taken place by the old scan, %d disagreements\n", yes, disagree);
        Check("the grid and the old scan agree on every query", disagree == 0 && yes > 1000);
    }

    printf("\n-- Decide takes off what it should and nothing else --\n");
    {
        Take(100);
        std::vector<gs::clearcheck::Pin> pins(5);
        const Place& t = g_takenList[3];
        pins[0] = {1, t.x + 1.0f, t.y + 2.0f, t.z, "Glint"};          // on a taken placement
        pins[1] = {2, t.x + 4.0f, t.y, t.z, "Glint"};                 // four metres off it
        pins[2] = {3, t.x, t.y + 9.0f, t.z, "Glint"};                 // nine metres above it
        pins[3] = {4, 50.0f, 0.0f, 0.0f, "pin"};                     // a key pin 50 m off
        pins[4] = {5, 5.0f, 0.0f, 0.0f, "821m"};                     // a key pin dropped beside him
        std::unordered_map<int64_t, bool> armed;
        std::vector<gs::clearcheck::Done> out(8);
        gs::clearcheck::Inputs in;
        in.clearTaken = true;
        in.clearNear = 10.0f;
        int n = gs::clearcheck::Decide(pins.data(), 5, in, NewTakenNear, armed, out.data(), 8);
        Check("only the Glint pin on the taken placement goes", n == 1 && out[0].pin.id == 1);
        in.px = 45.0f;    // walks up to the far pin, which was armed
        n = gs::clearcheck::Decide(pins.data(), 5, in, NewTakenNear, armed, out.data(), 8);
        Check("a key pin goes once he walks back to it", n == 2 && out[1].pin.id == 4 &&
                                                            strstr(out[1].why, "5 metres") != nullptr);
        // The one dropped beside him stayed through the first pass, armed while
        // he stood 40 m off, and goes when he comes back to it.
        in.px = 0.0f;
        n = gs::clearcheck::Decide(pins.data(), 5, in, NewTakenNear, armed, out.data(), 8);
        Check("one dropped beside him goes after he has been away", n == 2 && out[1].pin.id == 5);
        in.clearTaken = false;
        n = gs::clearcheck::Decide(pins.data(), 5, in, NewTakenNear, armed, out.data(), 8);
        Check("ClearTaken=0 leaves Glint pins alone", n == 0);
    }

    printf("\n-- timing, %d placements --\n", kPlaces);
    printf("      %-34s %12s %12s %8s\n", "case", "old loop ms", "grid ms", "removed");
    struct Case { int pins, taken; };
    const Case cases[] = {{21, 50}, {256, 50}, {256, 1811}, {256, kPlaces}, {1024, kPlaces}};
    double worstNew = 0, old256 = 0, new256 = 0;
    for (const Case& c : cases)
    {
        Take(c.taken);
        const auto pins = Pins(c.pins);
        std::vector<gs::clearcheck::Done> out;
        const int oldGot = OldPass(pins), newGot = NewPass(pins, out);
        const double o = Best(c.taken > 5000 ? 5 : 50, [&] { OldPass(pins); });
        const double n = Best(50, [&] { NewPass(pins, out); });
        char what[64];
        _snprintf_s(what, sizeof(what), _TRUNCATE, "%d pins, %d taken", c.pins, c.taken);
        printf("      %-34s %12.3f %12.3f %8d\n", what, o, n, newGot);
        if (oldGot != newGot)
        {
            ++failures;
            printf("FAIL  %s: the old loop takes off %d, the grid %d\n", what, oldGot, newGot);
        }
        if (c.pins == 256 && c.taken == kPlaces) { old256 = o; new256 = n; }
        worstNew = std::max(worstNew, n);
    }
    printf("      256 pins with every placement taken: %.3f ms before, %.3f ms after\n", old256, new256);
    Check("every case decides in under 2 ms on the key thread", worstNew < 2.0);

    printf("\n%s\n", failures ? "SOMETHING FAILED" : "all cases pass");
    return failures ? 1 : 0;
}
