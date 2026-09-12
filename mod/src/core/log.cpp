#include "core/log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
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

    // Shuffle GlintSpotter.log down to .01 and each .NN one further, dropping the
    // oldest. Done once at startup, before the file is opened.
    void Rotate(const std::wstring& base)
    {
        constexpr int kKeep = 5;
        auto numbered = [&](int i)
        {
            wchar_t suffix[16]{};
            swprintf_s(suffix, L".%02d.log", i);
            std::wstring p = base;
            const size_t dot = p.find_last_of(L'.');
            if (dot != std::wstring::npos) p.resize(dot);
            return p + suffix;
        };

        DeleteFileW(numbered(kKeep).c_str());
        for (int i = kKeep - 1; i >= 1; --i)
            MoveFileExW(numbered(i).c_str(), numbered(i + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(base.c_str(), numbered(1).c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

namespace gs::Log
{
    void Start(void* selfModule)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file) return;

        g_path = LogPathFor(static_cast<HMODULE>(selfModule));
        if (g_path.empty()) return;

        Rotate(g_path);
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
