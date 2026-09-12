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

    using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);

    CreateFileWFn g_realCreateFileW = nullptr;
    CreateFile2Fn g_realCreateFile2 = nullptr;
    ReadFileFn g_realReadFile = nullptr;

    // The game opened all eight of this machine's saves inside one tick, so
    // the queue holds far more than a load and a save now. It drains from the
    // front and keeps what does not fit in the caller's buffer.
    constexpr int kQueue = 64;
    std::mutex g_mutex;
    gs::saveslot::Event g_queue[kQueue];
    int g_queued = 0;
    bool g_dropped = false;

    // Save files with a handle open, and how much of each has been read. Small
    // and fixed: only save.save goes in, and an entry leaves as soon as it has
    // read enough to count as a load.
    struct Open
    {
        HANDLE handle = nullptr;
        gs::saveslot::Id id;
        uint64_t need = 0;    // bytes that make this a load rather than a look
        uint64_t got = 0;
        uint32_t since = 0;   // when it was opened, so it can be given up on
    };
    constexpr int kOpen = 16;
    Open g_open[kOpen];
    int g_openNext = 0;
    std::atomic<int> g_tracked{0};   // read by every ReadFile in the game

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

    // .../save/<digits>/slot<digits>/save.save, and nothing else. The game
    // builds this path with forward slashes, which is what its own format
    // string says and what the first session with this watch proved by
    // matching nothing at all, so neither separator can be assumed.
    bool Parse(const wchar_t* path, gs::saveslot::Id* out)
    {
        const size_t len = wcslen(path);
        if (!TailIs(path, len, L"save.save", 9)) return false;

        const wchar_t* file = Back(path, path + len);       // "save.save"
        if (file <= path) return false;
        if (file != path + len - 9) return false;           // not "mysave.save"
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

    // Called with the lock held.
    void PushLocked(const gs::saveslot::Id& id, bool write, bool full)
    {
        if (g_queued >= kQueue) { g_dropped = true; return; }
        g_queue[g_queued].id = id;
        g_queue[g_queued].write = write;
        g_queue[g_queued].full = full;
        ++g_queued;
    }

    void Push(const gs::saveslot::Id& id, bool write, bool full)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        PushLocked(id, write, full);
    }

    // A handle on a save file, and the size that says it was loaded rather
    // than glanced at. Half the file, which no header read comes near.
    void Track(HANDLE h, const gs::saveslot::Id& id)
    {
        if (!h || h == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        const uint64_t bytes = GetFileSizeEx(h, &size) && size.QuadPart > 0
                                   ? static_cast<uint64_t>(size.QuadPart)
                                   : 0;
        std::lock_guard<std::mutex> lock(g_mutex);
        Open& slot = g_open[g_openNext];
        g_openNext = (g_openNext + 1) % kOpen;
        slot.handle = h;
        slot.id = id;
        slot.need = bytes ? bytes / 2 : 256u * 1024u;
        slot.got = 0;
        slot.since = GetTickCount();
        g_tracked.store(1);
    }

    // Handles that were opened, read a little of, and closed without this ever
    // hearing about it. Left in the table they would keep the read counter
    // switched on, and that counter sits in front of every read the game
    // makes. Called from the tick, never from a detour.
    void Expire()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_tracked.load()) return;
        const uint32_t now = GetTickCount();
        bool any = false;
        for (Open& slot : g_open)
        {
            if (!slot.handle) continue;
            if (now - slot.since > 60000) slot.handle = nullptr;
            else any = true;
        }
        if (!any) g_tracked.store(0);
    }

    void Counted(HANDLE h, DWORD bytes)
    {
        if (!bytes) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Open& slot : g_open)
        {
            if (slot.handle != h) continue;
            slot.got += bytes;
            if (slot.got < slot.need) return;
            PushLocked(slot.id, false, true);
            slot.handle = nullptr;   // said its piece
            bool any = false;
            for (const Open& o : g_open) any = any || o.handle != nullptr;
            if (!any) g_tracked.store(0);
            return;
        }
    }

    // Writing a log line can open a file, and opening a file arrives back
    // here. Nothing this looks at lives in the save folder, so it has never
    // happened, but one thread-local byte is cheaper than finding out.
    thread_local bool g_inside = false;

    // The flag is set and cleared by hand rather than by a small object with a
    // destructor, because a destructor and __try cannot share a function. The
    // filter swallows everything, so the clear below is always reached.
    void Note(LPCWSTR path, DWORD access, DWORD disposition, HANDLE opened)
    {
        if (!path || g_inside) return;
        g_inside = true;
        gs::saveslot::Id id;
        __try
        {
            if (Parse(path, &id))
            {
                const bool write = Writing(access, disposition);
                Push(id, write, false);
                if (!write) Track(opened, id);
            }
            else if (g_oddLogsLeft.load() > 0 && TailIs(path, wcslen(path), L".save", 5))
            {
                --g_oddLogsLeft;
                GS_LOG("[save] a .save file this does not recognise: %ls (access 0x%08lX, "
                       "disposition %lu)", path, access, disposition);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        g_inside = false;
    }

    // The path is looked at after the call, not before, because the handle is
    // half the answer: what the game does with it is what says whether this is
    // the save being loaded.
    HANDLE WINAPI DetourCreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                    LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                                    HANDLE tmpl)
    {
        const HANDLE h = g_realCreateFileW(name, access, share, sa, disposition, flags, tmpl);
        Note(name, access, disposition, h);
        return h;
    }

    HANDLE WINAPI DetourCreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
                                    LPCREATEFILE2_EXTENDED_PARAMETERS params)
    {
        const HANDLE h = g_realCreateFile2(name, access, share, disposition, params);
        Note(name, access, disposition, h);
        return h;
    }

    // Every read the game makes comes through here, so the first thing it does
    // is an atomic load that is zero whenever no save file is open, which is
    // all of the time except the second or two around a load.
    BOOL WINAPI DetourReadFile(HANDLE h, LPVOID buf, DWORD want, LPDWORD got, LPOVERLAPPED ov)
    {
        const BOOL ok = g_realReadFile(h, buf, want, got, ov);
        if (ok && got && g_tracked.load()) Counted(h, *got);
        return ok;
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
        gs::iathook::Swap("KERNEL32.dll", "ReadFile", &DetourReadFile,
                          reinterpret_cast<void**>(&g_realReadFile));

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
        if (g_realReadFile)
        {
            gs::iathook::Swap("KERNEL32.dll", "ReadFile",
                              reinterpret_cast<void*>(g_realReadFile), &mine);
            g_realReadFile = nullptr;
        }
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
        Expire();
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_dropped)
        {
            g_dropped = false;
            GS_LOG_ERR("[save] more save opens arrived than the queue holds; one was missed");
        }
        int got = 0;
        for (; got < g_queued && got < n; ++got) out[got] = g_queue[got];
        // What did not fit stays, in order. The first build cleared the queue
        // whatever it had handed over, and the game opening eight saves in one
        // tick meant the one that mattered went in the half thrown away.
        for (int i = got; i < g_queued; ++i) g_queue[i - got] = g_queue[i];
        g_queued -= got;
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
