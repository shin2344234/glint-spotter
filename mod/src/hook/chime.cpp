#include "hook/chime.h"

#include <Windows.h>
#include <mmsystem.h>

#include <atomic>
#include <string>

#include "core/log.h"
#include "core/settings.h"

namespace
{
    std::atomic<bool> g_asked{false};

    using PlaySoundFn = BOOL(WINAPI*)(LPCWSTR, HMODULE, DWORD);
    PlaySoundFn g_play = nullptr;
    bool g_tried = false;
    std::wstring g_file;       // the full path to play, empty for the Asterisk sound
    bool g_playedSaid = false;
    bool g_failSaid = false;

    std::wstring Widen(const char* s)
    {
        // The ini is read as bytes. UTF-8 first, since that is what a modern
        // editor writes; the old code page when the bytes are not UTF-8.
        for (UINT cp : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP)})
        {
            const DWORD flags = cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
            const int n = MultiByteToWideChar(cp, flags, s, -1, nullptr, 0);
            if (n <= 0) continue;
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(cp, flags, s, -1, &w[0], n);
            w.resize(static_cast<size_t>(n - 1));
            return w;
        }
        return std::wstring();
    }

    std::wstring PluginFolder()
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&PluginFolder), &self))
            return std::wstring();
        wchar_t buf[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
        if (!n || n >= MAX_PATH) return std::wstring();
        std::wstring p(buf, n);
        const size_t cut = p.find_last_of(L"\\/");
        return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut + 1);
    }

    bool Ready()
    {
        if (g_tried) return g_play != nullptr;
        g_tried = true;

        // winmm from System32 by its full path. The ASI loader in bin64 is
        // itself a winmm.dll, and asking for the bare name would get that one.
        wchar_t sys[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n && n < MAX_PATH)
        {
            const std::wstring path = std::wstring(sys, n) + L"\\winmm.dll";
            if (HMODULE h = LoadLibraryW(path.c_str()))
                g_play = reinterpret_cast<PlaySoundFn>(GetProcAddress(h, "PlaySoundW"));
        }
        if (!g_play)
        {
            GS_LOG_ERR("sound: winmm's PlaySoundW is not there, so Sound=1 plays nothing");
            return false;
        }

        const char* name = gs::Settings::Get().soundFile;
        if (name[0])
        {
            std::wstring w = Widen(name);
            // A path with no drive and no leading slash is taken from the
            // plugin's own folder, which is bin64, not from wherever the
            // game's current directory happens to be.
            const bool full = (w.size() > 1 && w[1] == L':') ||
                              (!w.empty() && (w[0] == L'\\' || w[0] == L'/'));
            if (!full) w = PluginFolder() + w;
            if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES)
                GS_LOG_ERR("sound: SoundFile=%s is not a file I can find (looked for %ls), so a pin "
                           "plays the Windows Asterisk sound instead", name, w.c_str());
            else
                g_file = w;
        }
        GS_LOG("sound: a pin plays %ls", g_file.empty() ? L"the Windows Asterisk sound" : g_file.c_str());
        return true;
    }
}

namespace gs::chime
{
    void Ring() { g_asked.store(true); }

    void Pump()
    {
        if (!g_asked.exchange(false)) return;
        if (!Ready()) return;
        // SND_NODEFAULT, so a missing sound is silence rather than the
        // Windows default beep in its place.
        const BOOL ok = g_file.empty()
            ? g_play(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT)
            : g_play(g_file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        if (ok && !g_playedSaid)
        {
            g_playedSaid = true;
            GS_LOG("sound: played for the first pin this session");
        }
        else if (!ok && !g_failSaid)
        {
            g_failSaid = true;
            GS_LOG_ERR("sound: PlaySoundW refused the sound. A .wav it cannot decode does this, and so "
                       "does a sound scheme with nothing set for Asterisk.");
        }
    }
}
