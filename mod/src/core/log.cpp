#include "core/log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>
#include <atomic>
#include <mutex>
#include <string>

namespace
{
    // Lines go into g_buf under g_mutex and nothing else happens on the
    // caller's thread. The log's own thread writes them out ten times a
    // second, and at once after an error line. Through 1.1.41 every line was
    // written and flushed under one lock from whatever thread logged, the
    // game's included, and on 15 November the game thread's pulse and the
    // entity set both stood still for about a second at once during a world
    // load (GitHub #24).
    std::mutex g_mutex;            // g_buf, g_dropped, g_file being open
    std::mutex g_fileMutex;        // the writes to g_file, and g_out
    FILE* g_file = nullptr;
    std::wstring g_path;
    std::string g_buf;
    std::string g_out;
    uint64_t g_dropped = 0;
    HANDLE g_wake = nullptr;
    HANDLE g_flusher = nullptr;
    std::atomic<bool> g_stop{false};

    constexpr size_t kBufBytes = 1u << 20;     // a flush is asked for past this
    constexpr size_t kBufLimit = 64u << 20;    // lines are counted, not kept, past this
    constexpr DWORD kFlushMs = 100;

    // Everything buffered so far, onto the disk. Only ever one writer, and
    // the buffer lock is held just long enough to swap.
    void Drain()
    {
        std::lock_guard<std::mutex> file(g_fileMutex);
        uint64_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_out.swap(g_buf);
            dropped = g_dropped;
            g_dropped = 0;
        }
        if (g_file && !g_out.empty())
        {
            fwrite(g_out.data(), 1, g_out.size(), g_file);
            if (dropped)
                fprintf(g_file, "[--:--:--.---] error log: the disk fell behind and %llu line(s) "
                        "past %u MB waiting were not kept\n", static_cast<unsigned long long>(dropped),
                        static_cast<unsigned>(kBufLimit >> 20));
            fflush(g_file);
        }
        g_out.clear();   // keeps its capacity, which the next swap hands back
    }

    DWORD WINAPI Flusher(LPVOID)
    {
        while (!g_stop.load())
        {
            WaitForSingleObject(g_wake, kFlushMs);
            Drain();
        }
        return 0;
    }

    std::wstring LogPathFor(HMODULE self)
    {
        wchar_t buf[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return L"";

        std::wstring p(buf, n);
        const size_t dot = p.find_last_of(L'.');
        const size_t slash = p.find_last_of(L"\\/");
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
            p.resize(dot);
        return p + L".log";
    }

    // Logs= from GlintSpotter.ini, read here because the log is rotated before
    // gs::Settings::Load runs and Load itself writes to the log. The same rules
    // as Load's reader: comments and section lines skipped, the key matched
    // without regard to case, and a value outside 1 to 99 left at the default.
    // Keep the default and the range in step with settings.h and settings.cpp.
    constexpr int kLogsDefault = 2;
    constexpr int kLogsMax = 99;

    struct LogsRead { int logs; bool found; bool refused; };

    LogsRead ReadLogsSetting(const std::wstring& logPath)
    {
        LogsRead r{kLogsDefault, false, false};
        std::wstring ini = logPath;
        const size_t dot = ini.find_last_of(L'.');
        if (dot != std::wstring::npos) ini.resize(dot);
        ini += L".ini";

        FILE* f = nullptr;
        if (_wfopen_s(&f, ini.c_str(), L"r") != 0 || !f) return r;
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            const char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (_strnicmp(p, "Logs", 4) != 0) continue;
            p += 4;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p != '=') continue;
            const long n = atol(p + 1);
            r.found = true;
            r.refused = n < 1 || n > kLogsMax;
            r.logs = r.refused ? kLogsDefault : static_cast<int>(n);
        }
        fclose(f);
        return r;
    }

    // Shuffle GlintSpotter.log down to .01 and each .NN one further, keeping
    // `logs` files in all, this run's included, and deleting every older one,
    // so lowering Logs also clears what an earlier build left behind. Done
    // once at startup, before the file is opened.
    void Rotate(const std::wstring& base, int logs)
    {
        const int kKeep = logs - 1;
        auto numbered = [&](int i)
        {
            wchar_t suffix[16]{};
            swprintf_s(suffix, L".%02d.log", i);
            std::wstring p = base;
            const size_t dot = p.find_last_of(L'.');
            if (dot != std::wstring::npos) p.resize(dot);
            return p + suffix;
        };

        for (int i = kLogsMax; i >= kKeep && i >= 1; --i)
            DeleteFileW(numbered(i).c_str());
        for (int i = kKeep - 1; i >= 1; --i)
            MoveFileExW(numbered(i).c_str(), numbered(i + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
        if (kKeep >= 1)
            MoveFileExW(base.c_str(), numbered(1).c_str(), MOVEFILE_REPLACE_EXISTING);
        // With Logs=1 the "w" open below truncates the old file in place.
    }
}

namespace gs::Log
{
    void Start(void* selfModule)
    {
        LogsRead kept{};
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_file) return;

            g_path = LogPathFor(static_cast<HMODULE>(selfModule));
            if (g_path.empty()) return;

            kept = ReadLogsSetting(g_path);
            Rotate(g_path, kept.logs);
            // Plain bytes, no ccs= mode. Opening with ccs=UTF-8 puts the stream in
            // the CRT's Unicode mode, and every narrow call on a Unicode-mode stream
            // is an invalid parameter. The default handler answers that with
            // __fastfail, so the first fprintf below took the whole process down with
            // STATUS_STACK_BUFFER_OVERRUN and left this file holding nothing but the
            // byte order mark. Everything written here is ASCII, so bytes are enough.
            // _wfopen_s opens with deny-read sharing, so the log could not be read
            // while the game was running and the first session had to be played
            // twice. _SH_DENYWR keeps other writers out and lets readers in.
            g_file = _wfsopen(g_path.c_str(), L"w", _SH_DENYWR);
            if (!g_file) return;
            g_buf.reserve(kBufBytes + 4096);
            g_out.reserve(kBufBytes + 4096);
        }
        g_stop.store(false);
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_wake) g_flusher = CreateThread(nullptr, 0, Flusher, nullptr, 0, nullptr);

        if (kept.refused)
            Write("error", "log: Logs in the ini is out of range (1 to %d), keeping %d logs", kLogsMax,
                  kept.logs);
        if (kept.logs == 1)
            Write("info ", "log: Logs=1, only this run's log is kept and the older ones were deleted");
        else if (kept.logs == 2)
            Write("info ", "log: Logs=2%s, this run's log and the one before it, .01, are kept",
                  kept.found ? "" : " (the default)");
        else
            Write("info ", "log: Logs=%d%s, this run's log and the %d before it, .01 to .%02d, are kept",
                  kept.logs, kept.found ? "" : " (the default)", kept.logs - 1, kept.logs - 1);
    }

    void Write(const char* level, const char* fmt, ...)
    {
        // Formatted before the lock, on the stack when it fits and on the
        // heap when it does not, so a long line is never cut.
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char stack[1024];
        int head = _snprintf_s(stack, sizeof(stack), _TRUNCATE, "[%02d:%02d:%02d.%03d] %s ",
                               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);
        if (head < 0) head = 0;

        va_list args;
        va_start(args, fmt);
        va_list again;
        va_copy(again, args);
        const int body = vsnprintf(nullptr, 0, fmt, args);
        va_end(args);
        if (body < 0) { va_end(again); return; }

        std::string heap;
        char* line = stack;
        const size_t need = static_cast<size_t>(head) + static_cast<size_t>(body) + 2;
        if (need > sizeof(stack))
        {
            heap.assign(stack, static_cast<size_t>(head));
            heap.resize(need);
            line = &heap[0];
        }
        vsnprintf(line + head, static_cast<size_t>(body) + 1, fmt, again);
        va_end(again);
        line[head + body] = '\n';
        const size_t len = static_cast<size_t>(head + body + 1);

        // An error line goes out straight away, since it is the one a crash
        // report needs, and a full buffer does too.
        const bool error = level && strncmp(level, "error", 5) == 0;
        bool wake = error;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_file) return;
            if (g_buf.size() + len > kBufLimit) { ++g_dropped; return; }
            g_buf.append(line, len);
            if (g_buf.size() >= kBufBytes) wake = true;
        }
        if (wake && g_wake) SetEvent(g_wake);
    }

    void Shutdown()
    {
        // On process teardown the flusher is already gone, and a lock some
        // dead thread held stays held, so nothing here waits for anything.
        g_stop.store(true);
        if (g_flusher)
        {
            if (g_wake) SetEvent(g_wake);
            WaitForSingleObject(g_flusher, 1000);
            CloseHandle(g_flusher);
            g_flusher = nullptr;
        }
        if (!g_fileMutex.try_lock()) return;
        std::unique_lock<std::mutex> file(g_fileMutex, std::adopt_lock);
        if (!g_mutex.try_lock()) return;
        std::unique_lock<std::mutex> lock(g_mutex, std::adopt_lock);
        if (!g_file) return;
        if (!g_buf.empty()) fwrite(g_buf.data(), 1, g_buf.size(), g_file);
        g_buf.clear();
        fclose(g_file);
        g_file = nullptr;
        if (g_wake) { CloseHandle(g_wake); g_wake = nullptr; }
    }
}
