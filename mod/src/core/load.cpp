#include "core/load.h"

#include <atomic>
#include <cstdio>

#include "core/log.h"

namespace
{
    struct Acc
    {
        std::atomic<uint64_t> calls{0};
        std::atomic<int64_t> ticks{0};
        std::atomic<int64_t> worst{0};
    };

    Acc g_sites[gs::load::kSiteCount];
    std::atomic<int64_t> g_last[gs::load::kSiteCount];
    std::atomic<uint64_t> g_queries{0};
    std::atomic<uint64_t> g_frames{0};

    struct Thread
    {
        const char* name;
        HANDLE h;
        uint64_t lastCpu100ns;
    };
    constexpr int kThreads = 8;
    Thread g_threads[kThreads];
    std::atomic<int> g_threadCount{0};

    const char* const kSiteNames[gs::load::kSiteCount] = {
        "the tick's quarter second pulse", "  its save events", "  putting pins back", "  queued pins",
        "  dropped pins", "  the automatic marker", "  taking done pins off",
        "    its view ray", "    its bearing over the entity set", "    its bearing over the glint table",
        "    asking the game's detect system", "placing a pin",
        "a press answered on the tick", "the world map update", "the icon create spy", "the icon remove spy",
        "the pick up hook",
        "the entity set", "  walking the pools", "finding the player again", "the pad and the chord",
        "reading the save records", "the live state and pickups", "the taken set and state map",
        "the glint table",
    };

    // VirtualQuery calls by thread, so the report says whose they are.
    struct QueryCount
    {
        std::atomic<DWORD> tid{0};
        std::atomic<uint64_t> n{0};
        const char* name = nullptr;
    };
    constexpr int kQueryThreads = 16;
    QueryCount g_byThread[kQueryThreads];

    QueryCount* QuerySlot(DWORD tid)
    {
        for (int i = 0; i < kQueryThreads; ++i)
        {
            DWORD t = g_byThread[i].tid.load();
            if (t == tid) return &g_byThread[i];
            if (t == 0)
            {
                DWORD zero = 0;
                if (g_byThread[i].tid.compare_exchange_strong(zero, tid)) return &g_byThread[i];
                if (zero == tid) return &g_byThread[i];
            }
        }
        return nullptr;
    }

    int64_t Freq()
    {
        static int64_t f = 0;
        if (!f)
        {
            LARGE_INTEGER q;
            QueryPerformanceFrequency(&q);
            f = q.QuadPart;
        }
        return f;
    }

    uint64_t Cpu100ns(HANDLE h)
    {
        FILETIME c, e, k, u;
        if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
        const uint64_t kk = (static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime;
        const uint64_t uu = (static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime;
        return kk + uu;
    }

    uint32_t g_lastReportMs = 0;
}

namespace gs::load
{
    Timer::Timer(Site s) : site_(s) { QueryPerformanceCounter(&start_); }

    Timer::~Timer()
    {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        Book(site_, end.QuadPart - start_.QuadPart);
    }

    void Book(Site s, int64_t ticks)
    {
        Acc& a = g_sites[s];
        g_last[s].store(ticks);
        ++a.calls;
        a.ticks += ticks;
        int64_t w = a.worst.load();
        while (ticks > w && !a.worst.compare_exchange_weak(w, ticks)) {}
    }

    double LastMs(Site s) { return 1000.0 * static_cast<double>(g_last[s].load()) / static_cast<double>(Freq()); }

    void ClearLast(Site s) { g_last[s].store(0); }

    void Frame() { ++g_frames; }

    void CountQuery()
    {
        ++g_queries;
        if (QueryCount* q = QuerySlot(GetCurrentThreadId())) ++q->n;
    }

    void NameThisThread(const char* name)
    {
        if (QueryCount* q = QuerySlot(GetCurrentThreadId())) q->name = name;
    }

    void AddThread(const char* name, HANDLE h)
    {
        if (!h) return;
        // A copy of our own, so the owner closing its handle at teardown
        // cannot leave this one pointing at nothing.
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup,
                             THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
            return;
        const int i = g_threadCount.load();
        if (i >= kThreads) { CloseHandle(dup); return; }
        g_threads[i] = {name, dup, Cpu100ns(dup)};
        g_threadCount.store(i + 1);
        if (QueryCount* q = QuerySlot(GetThreadId(dup))) q->name = name;
    }

    // Once a minute, from the key thread.
    void Report(uint32_t nowMs)
    {
        if (!g_lastReportMs) { g_lastReportMs = nowMs; return; }
        const uint32_t span = nowMs - g_lastReportMs;
        if (span < 60000) return;
        g_lastReportMs = nowMs;

        const double f = static_cast<double>(Freq());
        const uint64_t frames = g_frames.exchange(0);
        GS_LOG("[load] the last %.0f s: %llu minimap ticks (%.0f a second), %llu VirtualQuery calls by the mod",
               span / 1000.0, static_cast<unsigned long long>(frames), frames * 1000.0 / span,
               static_cast<unsigned long long>(g_queries.exchange(0)));
        for (int s = 0; s < kSiteCount; ++s)
        {
            Acc& a = g_sites[s];
            const uint64_t calls = a.calls.exchange(0);
            const int64_t ticks = a.ticks.exchange(0);
            const int64_t worst = a.worst.exchange(0);
            if (!calls) continue;
            GS_LOG("[load]   %s, %s: %llu call(s), %.2f ms in all, %.3f ms each, worst %.2f ms",
                   s < kGameSites ? "game thread" : "mod thread", kSiteNames[s],
                   static_cast<unsigned long long>(calls), 1000.0 * ticks / f,
                   1000.0 * ticks / f / static_cast<double>(calls), 1000.0 * worst / f);
        }
        {
            char q[400];
            int w = 0;
            for (int i = 0; i < kQueryThreads && w + 50 < static_cast<int>(sizeof(q)); ++i)
            {
                if (!g_byThread[i].tid.load()) continue;
                const uint64_t n = g_byThread[i].n.exchange(0);
                if (!n) continue;
                const int k = _snprintf_s(q + w, sizeof(q) - w, _TRUNCATE, "%s%s %llu", w ? ", " : "",
                                          g_byThread[i].name ? g_byThread[i].name : "other",
                                          static_cast<unsigned long long>(n));
                if (k < 0) break;
                w += k;
            }
            q[w] = 0;
            if (w) GS_LOG("[load]   VirtualQuery calls by thread: %s", q);
        }
        char line[400];
        int w = 0;
        const int n = g_threadCount.load();
        for (int i = 0; i < n && w + 60 < static_cast<int>(sizeof(line)); ++i)
        {
            const uint64_t now = Cpu100ns(g_threads[i].h);
            const uint64_t used = now - g_threads[i].lastCpu100ns;
            g_threads[i].lastCpu100ns = now;
            const int k = _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, "%s%s %.0f ms", i ? ", " : "",
                                      g_threads[i].name, used / 10000.0);
            if (k < 0) break;
            w += k;
        }
        line[w] = 0;
        if (n) GS_LOG("[load]   the mod's own threads, CPU used: %s", line);
    }
}
