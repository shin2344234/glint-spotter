#include "core/mod.h"

#include <Windows.h>
#include <atomic>
#include <cstdlib>
#include <vector>

#include "core/log.h"
#include "game/rtti.h"
#include "game/scan.h"
#include "game/signatures.h"
#include "version.h"

namespace
{
    std::atomic<bool> g_stop{false};
    HANDLE g_thread = nullptr;
    void* g_self = nullptr;

    struct Target
    {
        const char* label;
        uintptr_t rva;
        const char* decorated;
        uintptr_t vtable = 0;    // resolved and RTTI-verified
        void* object = nullptr;  // last confirmed instance
        size_t instances = 0;    // how many the last scan saw
    };

    // Ask the game module where it actually landed rather than assuming
    // 0x140000000. Relocations are stripped on this exe so it will be that in
    // practice, but a plugin should not be the thing that discovers otherwise.
    uintptr_t GameBase()
    {
        return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    }

    // __DATE__ and __TIME__ bake in when mod.cpp last compiled, and mod.cpp only
    // recompiles when its own dependencies change, so the banner goes stale while
    // the rest of the plugin moves. Master Looter's notes call this out after it
    // cost a test round there. Report what the file on disk actually says instead.
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

    // Confirm the address named in signatures.h still holds the class we expect.
    // A patched exe fails here and the probe reports it instead of scanning for
    // a pointer that now means something else.
    bool Verify(Target& t, uintptr_t base)
    {
        t.vtable = base + t.rva;
        const char* name = gs::rtti::VtableClassName(reinterpret_cast<void*>(t.vtable));
        if (!name)
        {
            GS_LOG_ERR("%s: no RTTI at base+0x%08llX, refusing to scan for it",
                       t.label, static_cast<unsigned long long>(t.rva));
            t.vtable = 0;
            return false;
        }
        if (!gs::rtti::VtableIs(reinterpret_cast<void*>(t.vtable), t.decorated))
        {
            GS_LOG_ERR("%s: base+0x%08llX is %s, expected %s. Game patched, signatures stale.",
                       t.label, static_cast<unsigned long long>(t.rva), name, t.decorated);
            t.vtable = 0;
            return false;
        }
        GS_LOG_OK("%s: vtable 0x%p verified as %s", t.label,
                  reinterpret_cast<void*>(t.vtable), name);
        return true;
    }

    // What the create path would need, read once when an object first turns up.
    void DescribeOnce(const Target& t)
    {
        if (!gs::rtti::Readable(t.object, gs::sig::kRootControlSize))
        {
            GS_LOG("  not readable for its full 0x%zX bytes, so this is a stale match rather than a live object",
                   gs::sig::kRootControlSize);
            return;
        }
        auto** vt = *reinterpret_cast<void***>(t.object);
        if (!gs::rtti::Readable(vt, (gs::sig::kSlotCreateIcon + 1) * sizeof(void*))) return;

        GS_LOG("  slot %d update      0x%p", gs::sig::kSlotUpdate, vt[gs::sig::kSlotUpdate]);
        GS_LOG("  slot %d create icon 0x%p", gs::sig::kSlotCreateIcon, vt[gs::sig::kSlotCreateIcon]);
    }

    // One walk of the address space covering every target that has no live
    // pointer yet. The steady state never reaches this: once both are found,
    // Recheck below keeps them without touching the address space again.
    void ScanFor(Target* targets, size_t count)
    {
        uintptr_t needles[8]{};
        int slotOf[8]{};
        size_t n = 0;
        for (size_t i = 0; i < count && n < 8; ++i)
        {
            if (targets[i].vtable && !targets[i].object)
            {
                needles[n] = targets[i].vtable;
                slotOf[n] = static_cast<int>(i);
                ++n;
            }
        }
        if (n == 0) return;

        std::vector<gs::scan::Hit> hits;
        const gs::scan::Report rep = gs::scan::FindPointers(needles, n, hits);

        GS_LOG("scan: %zu region(s), %llu MB, %llu ms%s", rep.regionsScanned,
               static_cast<unsigned long long>(rep.bytesScanned / (1024 * 1024)),
               static_cast<unsigned long long>(rep.microseconds / 1000),
               rep.budgetHit ? ", byte budget hit" : "");

        for (size_t i = 0; i < n; ++i)
        {
            Target& t = targets[slotOf[i]];
            size_t found = 0;
            void* first = nullptr;
            for (const gs::scan::Hit& h : hits)
            {
                if (h.needle != static_cast<int>(i)) continue;
                if (!first) first = h.object;
                ++found;
            }

            t.instances = found;
            if (!found)
            {
                GS_LOG("%s: not in memory yet", t.label);
                continue;
            }

            t.object = first;
            GS_LOG_OK("%s: %zu instance(s), first at 0x%p", t.label, found, first);

            // More than one matters. The create call takes a single controller,
            // so if the game keeps several we have to know which is live before
            // anything is ever called on one.
            if (found > 1)
            {
                size_t shown = 0;
                for (const gs::scan::Hit& h : hits)
                {
                    if (h.needle != static_cast<int>(i)) continue;
                    if (shown++ >= 8) break;
                    GS_LOG("  [%zu] 0x%p in %s region 0x%llX +0x%llX", shown - 1, h.object,
                           h.regionType == MEM_PRIVATE ? "private" : "mapped",
                           static_cast<unsigned long long>(h.regionBase),
                           static_cast<unsigned long long>(h.regionSize));
                }
            }
            DescribeOnce(t);
        }
    }

    // Cheap confirmation that a pointer we already have still carries its
    // vtable. Clearing it here is what schedules the next scan.
    bool Recheck(Target& t)
    {
        if (!t.object) return false;
        if (gs::scan::StillValid(t.object, t.vtable)) return true;

        GS_LOG("%s: 0x%p no longer carries its vtable, will scan again", t.label, t.object);
        t.object = nullptr;
        return false;
    }
}

namespace
{
    DWORD WINAPI Worker(LPVOID)
    {
        GS_LOG("Glint Spotter %s probe", GS_VERSION_STRING);
        LogBuildStamp(g_self);
        GS_LOG("Read-only. It looks for two UI objects and writes what it finds. No hooks, nothing called.");

        const uintptr_t base = GameBase();
        GS_LOG("game module at 0x%p", reinterpret_cast<void*>(base));

        Target targets[] = {
            {"world map", gs::sig::kWorldMapVtable, gs::sig::kWorldMapClass},
            {"minimap",   gs::sig::kMiniMapVtable,  gs::sig::kMiniMapClass},
        };
        constexpr size_t kCount = sizeof(targets) / sizeof(targets[0]);

        // The exe is mapped long before the UI exists, so the vtables can be
        // verified early even though nothing has been constructed from them yet.
        for (int i = 0; i < 20 && !g_stop.load(); ++i) Sleep(500);

        bool any = false;
        for (Target& t : targets) any |= Verify(t, base);
        if (!any)
        {
            GS_LOG_ERR("neither vtable verified, nothing further to do this session");
            return 0;
        }

        // Both controls are built when their page is first opened, so the early
        // passes are looking for something that does not exist yet. Once a
        // pointer is in hand the loop costs a couple of pointer reads, and it
        // only walks the address space again if one goes stale.
        int idle = 0;
        while (!g_stop.load())
        {
            size_t live = 0;
            for (Target& t : targets) live += Recheck(t) ? 1 : 0;

            if (live < kCount)
            {
                ScanFor(targets, kCount);
                idle = 0;
            }
            else if (++idle == 1)
            {
                GS_LOG("both pointers live, holding. Nothing more is written unless one changes.");
            }

            // Ten seconds while hunting. Once everything is found the body above
            // is two pointer reads, so the wait is only about noticing a reload.
            for (int i = 0; i < 20 && !g_stop.load(); ++i) Sleep(500);
        }
        return 0;
    }
}

namespace
{
    // The CRT answers an invalid parameter by calling __fastfail, which kills the
    // whole process. That is a defensible default for an application and a
    // terrible one for a plugin living in someone else's: opening the log in the
    // CRT's Unicode mode made the first narrow fprintf invalid, and the game died
    // at startup with STATUS_STACK_BUFFER_OVERRUN and no log to say why.
    //
    // The static CRT means this handler is ours alone and the game's own CRT is
    // untouched. Returning from it makes the offending call fail and set errno
    // instead of taking the process down.
    thread_local bool t_inHandler = false;

    void __cdecl OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                                    unsigned int, uintptr_t)
    {
        // The log is a plausible source of the fault, so never re-enter it.
        if (t_inHandler) return;
        t_inHandler = true;
        GS_LOG_ERR("CRT invalid parameter swallowed; a call failed but the process lives");
        t_inHandler = false;
    }

    void InstallInvalidParameterHandler()
    {
        _set_invalid_parameter_handler(OnInvalidParameter);
    }
}

namespace gs::Mod
{
    void Initialize(void* selfModule)
    {
        InstallInvalidParameterHandler();
        g_self = selfModule;
#if GS_STAGE >= 2
        Log::Start(selfModule);
#else
        (void)selfModule;
#endif
#if GS_STAGE >= 3
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
#endif
    }

    void Shutdown(bool processTerminating)
    {
        g_stop.store(true);
        // On process teardown the loader lock is held and other threads are
        // already gone, so waiting on one is how a plugin hangs an exit.
        if (!processTerminating && g_thread)
        {
            WaitForSingleObject(g_thread, 3000);
        }
        if (g_thread)
        {
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
        Log::Shutdown();
    }
}
