#include "core/mod.h"

#include <Windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "game/rtti.h"
#include "game/scan.h"
#include "game/signatures.h"
#include "game/typescan.h"
#include "version.h"

namespace
{
    std::atomic<bool> g_stop{false};
    HANDLE g_thread = nullptr;
    void* g_self = nullptr;

    // The keyword sweep finds 104 classes on build 2.01.00, so a cap of 48 was
    // silently dropping more than half of them.
    constexpr size_t kMaxClasses = 160;
    constexpr size_t kMaxNeedles = 64;

    // A class we are hunting, and the best pointer we have to a live one.
    struct Target
    {
        gs::typescan::ClassInfo info{};
        size_t objectBytes = 0x100;  // fit test; the root controls know better
        void* object = nullptr;
        size_t candidates = 0;
        bool hunt = false;           // false for template instantiations
    };

    Target g_targets[kMaxClasses];
    size_t g_count = 0;

    // Anything in the map icon and detect mode families. Both spellings of
    // minimap appear in this binary, so both are listed.
    const char* const kKeywords[] = {
        "MapIcon", "MiniMap", "Minimap", "WorldMap", "DetectMode", "SpecialMode",
    };

    const char* ShortName(const char* decorated)
    {
        // ".?AVFoo@bar@pa@@" reads better as "Foo@bar@pa@@" in a log.
        if (decorated && decorated[0] == '.' && decorated[1] == '?') return decorated + 4;
        return decorated ? decorated : "";
    }

    // The two classes whose size is actually known, from the factory that builds
    // them. A tight fit test is what threw out session one's false positives, so
    // it is worth being explicit about where the number is real and where it is
    // only a floor.
    size_t KnownSize(const char* decorated)
    {
        if (strstr(decorated, "UIGamePlayControlRootWorldMap") ||
            strstr(decorated, "UIGamePlayControlRootMiniMap"))
            return gs::sig::kRootControlSize;
        return 0x100;
    }

    void Discover()
    {
        uintptr_t base = 0;
        size_t size = 0;
        if (gs::typescan::ModuleRange(base, size))
            GS_LOG("game module 0x%p, %llu MB", reinterpret_cast<void*>(base),
                   static_cast<unsigned long long>(size / (1024 * 1024)));

        gs::typescan::ClassInfo found[kMaxClasses]{};
        const size_t n = gs::typescan::FindClasses(
            kKeywords, sizeof(kKeywords) / sizeof(kKeywords[0]), found, kMaxClasses);

        GS_LOG("RTTI: %zu class(es) in the map icon and detect mode families", n);

        for (size_t i = 0; i < n && g_count < kMaxClasses; ++i)
        {
            if (!found[i].vtableVa)
            {
                // Nothing points at the locator, so the class is abstract or only
                // ever appears inside a template. There is no object to look for.
                GS_LOG("  no vtable            %s", ShortName(found[i].name));
                continue;
            }
            Target& t = g_targets[g_count];
            t.info = found[i];
            t.objectBytes = KnownSize(found[i].name);
            // "?$" marks a template instantiation. Those are binders, pools and
            // reflection glue rather than the objects the create path uses, and
            // there are enough of them to crowd out the real classes. They are
            // still logged, because the argument lists in their names are how the
            // map icon event signature was read in the first place.
            t.hunt = ShortName(found[i].name)[0] != '?';
            GS_LOG("  %s +0x%08X %3d slots  %s", t.hunt ? "hunt" : "    ",
                   found[i].vtableRva, found[i].slots, ShortName(found[i].name));
            ++g_count;
        }

        // The two addresses static analysis produced, held up against what the
        // running game says. Disagreement means the exe moved and every RVA in
        // signatures.h is stale.
        struct Expect { uintptr_t rva; const char* name; };
        const Expect expected[] = {
            {gs::sig::kWorldMapVtable, gs::sig::kWorldMapClass},
            {gs::sig::kMiniMapVtable,  gs::sig::kMiniMapClass},
        };
        for (const Expect& e : expected)
        {
            bool agreed = false;
            for (size_t i = 0; i < g_count; ++i)
                if (g_targets[i].info.vtableRva == e.rva &&
                    strcmp(g_targets[i].info.name, e.name) == 0)
                    agreed = true;
            if (agreed)
                GS_LOG_OK("signatures.h +0x%08llX still matches %s",
                          static_cast<unsigned long long>(e.rva), ShortName(e.name));
            else
                GS_LOG_ERR("signatures.h +0x%08llX no longer matches %s, RVAs are stale",
                           static_cast<unsigned long long>(e.rva), ShortName(e.name));
        }
    }

    // Read what the create path would need. False means the candidate does not
    // stand up and the caller must drop it: session one cached a rejected
    // pointer, the next tick confirmed its vtable, and no scan ever ran again.
    bool Describe(Target& t)
    {
        if (!gs::rtti::Readable(t.object, t.objectBytes))
        {
            GS_LOG("    not readable for a whole object, dropped");
            return false;
        }

        // In a table of vtable pointers the neighbouring qword is another
        // vtable, and in an object it is member data. Session two found 112
        // candidates and every one was a table entry, so this test is what
        // separates the two.
        const auto* q = static_cast<const uintptr_t*>(t.object);
        if (gs::rtti::VtableClassName(reinterpret_cast<const void*>(q[1])))
        {
            GS_LOG("    +08 is itself a vtable, so this is a pointer table, dropped");
            return false;
        }

        auto** vt = *reinterpret_cast<void***>(t.object);
        const int want = gs::sig::kSlotCreateIcon + 1;
        if (t.info.slots >= want &&
            gs::rtti::Readable(vt, static_cast<size_t>(want) * sizeof(void*)))
        {
            GS_LOG("    slot %d update 0x%p, slot %d create icon 0x%p",
                   gs::sig::kSlotUpdate, vt[gs::sig::kSlotUpdate],
                   gs::sig::kSlotCreateIcon, vt[gs::sig::kSlotCreateIcon]);
        }

        // First bytes of the object. Cheap, and it is what will later tell us
        // whether two candidates are one control seen twice or two controls.
        const auto* b = static_cast<const uint8_t*>(t.object);
        GS_LOG("    +00 %02X%02X%02X%02X%02X%02X%02X%02X  +08 %02X%02X%02X%02X%02X%02X%02X%02X"
               "  +10 %02X%02X%02X%02X%02X%02X%02X%02X",
               b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
               b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15],
               b[16], b[17], b[18], b[19], b[20], b[21], b[22], b[23]);
        return true;
    }

    void LogReport(const char* label, const gs::scan::Report& rep)
    {
        GS_LOG("%s: %zu region(s) read, %llu MB, %llu ms%s", label, rep.regionsScanned,
               static_cast<unsigned long long>(rep.bytesScanned / (1024 * 1024)),
               static_cast<unsigned long long>(rep.microseconds / 1000),
               rep.timeBudgetHit ? ", TIME BUDGET HIT, coverage incomplete" : "");
        GS_LOG("  skipped %zu wrong kind, %zu too small, %zu too large (%llu MB)",
               rep.regionsSkippedKind, rep.regionsSkippedSmall, rep.regionsSkippedLarge,
               static_cast<unsigned long long>(rep.bytesSkippedLarge / (1024 * 1024)));
        GS_LOG("  %zu pointer match(es), %zu rejected for having no room behind them",
               rep.rawMatches, rep.rejectedNoRoom);
    }

    // A region that answers to several different classes at once is a table of
    // vtable pointers rather than a heap. One 48 KB region in session two held
    // hits for all 56 classes and every one of them was noise. Returns how many
    // hits are left standing.
    size_t DropPointerTables(std::vector<gs::scan::Hit>& hits)
    {
        constexpr size_t kTableThreshold = 4;
        for (size_t a = 0; a < hits.size(); ++a)
        {
            if (!hits[a].object) continue;
            int seen[kMaxNeedles]{};
            size_t distinct = 0;
            for (size_t b = a; b < hits.size(); ++b)
            {
                if (!hits[b].object || hits[b].regionBase != hits[a].regionBase) continue;
                bool already = false;
                for (size_t k = 0; k < distinct; ++k)
                    if (seen[k] == hits[b].needle) already = true;
                if (!already && distinct < kMaxNeedles) seen[distinct++] = hits[b].needle;
            }
            if (distinct < kTableThreshold) continue;

            size_t dropped = 0;
            const uintptr_t region = hits[a].regionBase;
            for (gs::scan::Hit& h : hits)
                if (h.regionBase == region && h.object) { h.object = nullptr; ++dropped; }
            GS_LOG("  region 0x%llX answers to %zu different classes, so it is a table "
                   "of vtable pointers; %zu hit(s) discarded",
                   static_cast<unsigned long long>(region), distinct, dropped);
        }

        size_t live = 0;
        for (const gs::scan::Hit& h : hits) live += h.object ? 1 : 0;
        return live;
    }

    // One walk covering every class with no live pointer yet. Narrow first
    // because it is fast, then wide in the same tick if everything came back
    // empty, because a session costs real time and coming back with nothing is
    // the expensive outcome.
    void ScanFor()
    {
        uintptr_t needles[kMaxClasses]{};
        size_t bytes[kMaxClasses]{};
        size_t slotOf[kMaxClasses]{};
        size_t n = 0;
        for (size_t i = 0; i < g_count; ++i)
        {
            if (!g_targets[i].hunt || g_targets[i].object) continue;
            if (n >= kMaxNeedles) break;
            needles[n] = g_targets[i].info.vtableVa;
            bytes[n] = g_targets[i].objectBytes;
            slotOf[n] = i;
            ++n;
        }
        if (n == 0) return;

        gs::scan::Options opt;
        opt.needleBytes = bytes;
        // Session two spent its four seconds on 509 MB and stopped there, so most
        // of the heap was never looked at. This runs only while something is
        // still missing, so a long pass costs a pause and not a stutter.
        opt.timeBudgetMs = 25000;

        std::vector<gs::scan::Hit> hits;
        gs::scan::Report rep = gs::scan::FindPointers(needles, n, hits, opt);
        LogReport("narrow scan", rep);

        // Tables have to be thrown out before deciding whether to widen, or a
        // hundred table entries read as success and the wide pass never runs.
        if (DropPointerTables(hits) == 0)
        {
            hits.clear();
            opt.wideKinds = true;
            opt.timeBudgetMs = 60000;
            opt.maxRegionBytes = 4ull * 1024 * 1024 * 1024;
            rep = gs::scan::FindPointers(needles, n, hits, opt);
            LogReport("wide scan", rep);
            DropPointerTables(hits);
        }

        for (size_t i = 0; i < n; ++i)
        {
            Target& t = g_targets[slotOf[i]];
            size_t found = 0;
            for (const gs::scan::Hit& h : hits)
                if (h.object && h.needle == static_cast<int>(i)) ++found;
            if (found == 0) continue;

            t.candidates = found;
            GS_LOG_OK("%s: %zu candidate(s)", ShortName(t.info.name), found);

            size_t shown = 0;
            for (const gs::scan::Hit& h : hits)
            {
                if (!h.object || h.needle != static_cast<int>(i)) continue;
                if (shown++ >= 6) break;
                GS_LOG("  [%zu] 0x%p in region 0x%llX +0x%llX", shown - 1, h.object,
                       static_cast<unsigned long long>(h.regionBase),
                       static_cast<unsigned long long>(h.regionSize));
                t.object = h.object;
                if (!Describe(t)) t.object = nullptr;
            }
            if (found > shown) GS_LOG("  ... %zu more", found - shown);
        }
    }

    bool Recheck(Target& t)
    {
        if (!t.object) return false;
        if (gs::scan::StillValid(t.object, t.info.vtableVa)) return true;
        GS_LOG("%s: 0x%p stopped carrying its vtable, will look again",
               ShortName(t.info.name), t.object);
        t.object = nullptr;
        return false;
    }

    // The CRT answers an invalid parameter by calling __fastfail, which kills the
    // whole process. That is a defensible default for an application and a
    // terrible one for a plugin living in someone else's: opening the log in the
    // CRT's Unicode mode made the first narrow fprintf invalid, and the game died
    // at startup with STATUS_STACK_BUFFER_OVERRUN and no log to say why.
    //
    // The static CRT means this handler is ours alone and the game's own is
    // untouched. Returning from it makes the offending call fail and set errno
    // instead of ending the process.
    thread_local bool t_inHandler = false;

    void __cdecl OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                                    unsigned int, uintptr_t)
    {
        if (t_inHandler) return;
        t_inHandler = true;
        GS_LOG_ERR("CRT invalid parameter swallowed; a call failed but the process lives");
        t_inHandler = false;
    }

    // __DATE__ and __TIME__ bake in when mod.cpp last compiled, and mod.cpp only
    // recompiles when its own dependencies change, so the banner goes stale while
    // the rest of the plugin moves. Master Looter's notes call this out after it
    // cost a test round there. Report what the file on disk says instead.
    void LogBuildStamp(void* selfModule)
    {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(static_cast<HMODULE>(selfModule), path, MAX_PATH)) return;

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;

        SYSTEMTIME utc{}, local{};
        FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);

        GS_LOG("plugin file written %04d-%02d-%02d %02d:%02d:%02d, %lu bytes",
               local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
               local.wSecond, fad.nFileSizeLow);
    }

    DWORD WINAPI Worker(LPVOID)
    {
        GS_LOG("Glint Spotter %s probe", GS_VERSION_STRING);
        LogBuildStamp(g_self);
        GS_LOG("Read-only. It reads the game's own RTTI, looks for those objects in");
        GS_LOG("memory, and writes what it finds. No hooks, nothing called.");

        // The exe is mapped long before the UI exists, so RTTI can be read early
        // even though nothing has been built from it yet.
        for (int i = 0; i < 16 && !g_stop.load(); ++i) Sleep(500);
        Discover();
        if (g_count == 0)
        {
            GS_LOG_ERR("no classes with vtables found, nothing to look for this session");
            return 0;
        }

        // Early passes hunt for something that may not exist yet, so they come
        // quickly. Once everything is in hand a tick is one pointer read each and
        // the address space is left alone.
        int pass = 0;
        int idle = 0;
        while (!g_stop.load())
        {
            size_t live = 0, hunted = 0;
            for (size_t i = 0; i < g_count; ++i)
            {
                if (!g_targets[i].hunt) continue;
                ++hunted;
                live += Recheck(g_targets[i]) ? 1 : 0;
            }

            if (live < hunted)
            {
                GS_LOG("--- pass %d, %zu of %zu located ---", ++pass, live, hunted);
                ScanFor();
                idle = 0;
            }
            else if (++idle == 1)
            {
                GS_LOG_OK("all %zu located, holding. Nothing more unless one changes.", hunted);
            }

            const int ticks = (pass < 6) ? 10 : 30;  // 5 s early, 15 s once settled
            for (int i = 0; i < ticks && !g_stop.load(); ++i) Sleep(500);
        }
        return 0;
    }
}

namespace gs::Mod
{
    void Initialize(void* selfModule)
    {
        _set_invalid_parameter_handler(OnInvalidParameter);
        g_self = selfModule;
        Log::Start(selfModule);
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }

    void Shutdown(bool processTerminating)
    {
        g_stop.store(true);
        // On process teardown the loader lock is held and other threads are
        // already gone, so waiting on one is how a plugin hangs an exit.
        if (!processTerminating && g_thread) WaitForSingleObject(g_thread, 3000);
        if (g_thread)
        {
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
        Log::Shutdown();
    }
}
