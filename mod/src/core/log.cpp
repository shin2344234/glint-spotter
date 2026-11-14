#include "core/log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>
#include <mutex>
#include <string>

namespace
{
    std::mutex g_mutex;
    FILE* g_file = nullptr;
    std::wstring g_path;

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
        }

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
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_file) return;

        SYSTEMTIME st{};
        GetLocalTime(&st);
        fprintf(g_file, "[%02d:%02d:%02d.%03d] %s ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);

        va_list args;
        va_start(args, fmt);
        vfprintf(g_file, fmt, args);
        va_end(args);

        fputc('\n', g_file);
        // Flushed every line on purpose. A crash during a probe is exactly when
        // the last line matters most, and this plugin writes a few lines a
        // minute rather than a few thousand.
        fflush(g_file);
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_file) return;
        fclose(g_file);
        g_file = nullptr;
    }
}
