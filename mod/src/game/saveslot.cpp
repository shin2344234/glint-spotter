#include "game/saveslot.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "hook/iathook.h"

namespace
{
    using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD,
                                          DWORD, HANDLE);
    using CreateFile2Fn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD,
                                          LPCREATEFILE2_EXTENDED_PARAMETERS);

    CreateFileWFn g_realCreateFileW = nullptr;
    CreateFile2Fn g_realCreateFile2 = nullptr;

    // Sixteen is more save opens than can happen between two ticks: a load is
    // one, a save is one, and the tick runs many times a second.
    constexpr int kQueue = 16;
    std::mutex g_mutex;
    gs::saveslot::Event g_queue[kQueue];
    int g_queued = 0;
    bool g_dropped = false;

    // A handful of lines about anything opened under the save folder that this
    // does not recognise. If the game ever writes through a temp file, or
    // renames one over another, the first session with this build says so
    // instead of leaving a silent gap.
    std::atomic<int> g_oddLogsLeft{8};

    bool TailIs(const wchar_t* path, size_t len, const wchar_t* want, size_t wantLen)
    {
        if (len < wantLen) return false;
        const wchar_t* at = path + (len - wantLen);
        for (size_t i = 0; i < wantLen; ++i)
        {
            wchar_t a = at[i], b = want[i];
            if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
            if (a != b) return false;
        }
        return true;
    }

    // Walk back over one path component and hand back where it starts.
    const wchar_t* Back(const wchar_t* start, const wchar_t* at)
    {
        while (at > start && at[-1] != L'\\' && at[-1] != L'/') --at;
        return at;
    }

    bool DigitsOnly(const wchar_t* from, const wchar_t* to, uint32_t* out)
    {
        if (from >= to) return false;
        uint32_t v = 0;
        for (const wchar_t* p = from; p < to; ++p)
        {
            if (*p < L'0' || *p > L'9') return false;
            if (v > 0xFFFFFFFFu / 10) return false;
            v = v * 10 + static_cast<uint32_t>(*p - L'0');
        }
        *out = v;
        return true;
    }

    // ...\save\<digits>\slot<digits>\save.save, and nothing else.
    bool Parse(const wchar_t* path, gs::saveslot::Id* out)
    {
        const size_t len = wcslen(path);
        if (!TailIs(path, len, L"\\save.save", 10)) return false;

        const wchar_t* file = Back(path, path + len);       // "save.save"
        if (file <= path) return false;
        const wchar_t* slotDir = Back(path, file - 1);      // "slot<N>"
        const wchar_t* slotEnd = file - 1;
        if (slotEnd - slotDir < 5) return false;
        if (_wcsnicmp(slotDir, L"slot", 4) != 0) return false;
        uint32_t slot = 0;
        if (!DigitsOnly(slotDir + 4, slotEnd, &slot)) return false;

        if (slotDir <= path) return false;
        const wchar_t* acctDir = Back(path, slotDir - 1);   // "<account>"
        const wchar_t* acctEnd = slotDir - 1;
        uint32_t account = 0;
        if (!DigitsOnly(acctDir, acctEnd, &account) || account == 0) return false;

        // One more component back has to be the save folder itself, so a
        // directory somebody happens to have called slot7 cannot be mistaken
        // for a save.
        if (acctDir <= path) return false;
        const wchar_t* saveDir = Back(path, acctDir - 1);
        if (_wcsnicmp(saveDir, L"save\\", 5) != 0 && _wcsnicmp(saveDir, L"save/", 5) != 0)
            return false;

        out->account = account;
        out->slot = static_cast<int32_t>(slot);
        return true;
    }

    bool Writing(DWORD access, DWORD disposition)
    {
        if (access & (GENERIC_WRITE | FILE_GENERIC_WRITE | GENERIC_ALL)) return true;
        return disposition == CREATE_ALWAYS || disposition == CREATE_NEW ||
               disposition == TRUNCATE_EXISTING;
    }

    void Push(const gs::saveslot::Id& id, bool write)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queued >= kQueue) { g_dropped = true; return; }
        g_queue[g_queued].id = id;
        g_queue[g_queued].write = write;
        ++g_queued;
    }

    // Writing a log line can open a file, and opening a file arrives back
    // here. Nothing this looks at lives in the save folder, so it has never
    // happened, but one thread-local byte is cheaper than finding out.
    thread_local bool g_inside = false;

    // The flag is set and cleared by hand rather than by a small object with a
    // destructor, because a destructor and __try cannot share a function. The
    // filter swallows everything, so the clear below is always reached.
    void Note(LPCWSTR path, DWORD access, DWORD disposition)
    {
        if (!path || g_inside) return;
        g_inside = true;
        gs::saveslot::Id id;
        __try
        {
            if (Parse(path, &id))
                Push(id, Writing(access, disposition));
            else if (g_oddLogsLeft.load() > 0 && wcsstr(path, L"\\CD\\save\\"))
            {
                --g_oddLogsLeft;
                GS_LOG("[save] something else under the save folder: %ls (access 0x%08lX, "
                       "disposition %lu)", path, access, disposition);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        g_inside = false;
    }

    HANDLE WINAPI DetourCreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                    LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                                    HANDLE tmpl)
    {
        Note(name, access, disposition);
        return g_realCreateFileW(name, access, share, sa, disposition, flags, tmpl);
    }

    HANDLE WINAPI DetourCreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
                                    LPCREATEFILE2_EXTENDED_PARAMETERS params)
    {
        Note(name, access, disposition);
        return g_realCreateFile2(name, access, share, disposition, params);
    }
}

namespace gs::saveslot
{
    bool Install()
    {
        // The forwarding pointer is the out parameter, so it holds the real
        // function before the detour is reachable rather than a moment after.
        gs::iathook::Swap("KERNEL32.dll", "CreateFileW", &DetourCreateFileW,
                          reinterpret_cast<void**>(&g_realCreateFileW));
        gs::iathook::Swap("KERNEL32.dll", "CreateFile2", &DetourCreateFile2,
                          reinterpret_cast<void**>(&g_realCreateFile2));

        if (!g_realCreateFileW && !g_realCreateFile2)
        {
            GS_LOG_ERR("[save] the game imports neither file-open call, so which save is loaded "
                       "cannot be read and every pin goes in one set");
            return false;
        }
        GS_LOG_OK("[save] watching the save folder through %s%s%s. Loading a save says which one "
                  "it is, and the pins for that save go on the map.",
                  g_realCreateFileW ? "CreateFileW" : "",
                  (g_realCreateFileW && g_realCreateFile2) ? " and " : "",
                  g_realCreateFile2 ? "CreateFile2" : "");
        return true;
    }

    void Remove()
    {
        // Same bargain the vtable slots take: a thread already inside the
        // detour when this runs still returns through it. Only reached on an
        // unload that leaves the game running, which is not something an ASI
        // loader does on its own.
        void* mine = nullptr;
        if (g_realCreateFileW)
        {
            gs::iathook::Swap("KERNEL32.dll", "CreateFileW",
                              reinterpret_cast<void*>(g_realCreateFileW), &mine);
            g_realCreateFileW = nullptr;
        }
        if (g_realCreateFile2)
        {
            gs::iathook::Swap("KERNEL32.dll", "CreateFile2",
                              reinterpret_cast<void*>(g_realCreateFile2), &mine);
            g_realCreateFile2 = nullptr;
        }
    }

    int Take(Event* out, int n)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        int got = 0;
        for (int i = 0; i < g_queued && got < n; ++i) out[got++] = g_queue[i];
        if (g_dropped)
        {
            g_dropped = false;
            GS_LOG_ERR("[save] more save opens arrived than the queue holds; one was missed");
        }
        g_queued = 0;
        return got;
    }

    void Text(const Id& id, char* out, int n)
    {
        if (!out || n <= 0) return;
        if (!id.ok()) { strncpy_s(out, static_cast<size_t>(n), "no save", _TRUNCATE); return; }
        uint32_t h = 2166136261u;
        for (uint32_t v = id.account; v; v >>= 8)
        {
            h ^= (v & 0xFF);
            h *= 16777619u;
        }
        sprintf_s(out, static_cast<size_t>(n), "slot%d of account %04X", id.slot,
                  static_cast<unsigned>(h & 0xFFFF));
    }
}
