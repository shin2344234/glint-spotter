#include "core/pinstore.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "core/log.h"
#include "core/settings.h"

namespace
{
    constexpr int kMax = 256;

    std::mutex g_mutex;
    gs::pinstore::Saved g_pins[kMax];
    int g_count = 0;
    std::wstring g_path;

    // Beside the plugin, named after it, the same way the log and the ini are.
    std::wstring PathFor(HMODULE self)
    {
        wchar_t buf[MAX_PATH]{};
        if (!GetModuleFileNameW(self, buf, MAX_PATH)) return L"";
        std::wstring p(buf);
        const size_t dot = p.find_last_of(L'.');
        const size_t slash = p.find_last_of(L"\\/");
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
            p.resize(dot);
        return p + L".pins";
    }

    // Called with the lock held.
    void Write()
    {
        if (g_path.empty()) return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_path.c_str(), L"w") != 0 || !f) return;
        fputs("; Glint Spotter's pins. One per line: x y z label.\n", f);
        fputs("; Delete this file to clear every pin.\n", f);
        for (int i = 0; i < g_count; ++i)
            fprintf(f, "%.3f %.3f %.3f %s\n", g_pins[i].x, g_pins[i].y, g_pins[i].z,
                    g_pins[i].label);
        fclose(f);
    }
}

namespace gs::pinstore
{
    void Load(void* selfModule)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_path = PathFor(static_cast<HMODULE>(selfModule));
        g_count = 0;
        if (g_path.empty()) return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_path.c_str(), L"r") != 0 || !f)
        {
            GS_LOG("[pins] no pin file yet; one is written the first time you mark something");
            return;
        }
        char line[256];
        while (g_count < kMax && fgets(line, sizeof(line), f))
        {
            if (line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
            Saved s{};
            char label[64]{};
            const int n = sscanf_s(line, "%f %f %f %63s", &s.x, &s.y, &s.z, label,
                                   static_cast<unsigned>(sizeof(label)));
            if (n < 3) continue;
            strncpy_s(s.label, sizeof(s.label), n >= 4 ? label : "Pin", _TRUNCATE);
            g_pins[g_count++] = s;
        }
        fclose(f);
        GS_LOG_OK("[pins] %d pin(s) read from the file beside the plugin; they go back on the map "
                  "when a world appears", g_count);
    }

    void Add(float x, float y, float z, const char* label)
    {
        if (!gs::Settings::Get().keepPins) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_count >= kMax)
        {
            GS_LOG_ERR("[pins] the pin file already holds %d; this one is not written down", kMax);
            return;
        }
        Saved& s = g_pins[g_count++];
        s.x = x; s.y = y; s.z = z;
        strncpy_s(s.label, sizeof(s.label), label && label[0] ? label : "Pin", _TRUNCATE);
        Write();
    }

    void Drop(float x, float z)
    {
        if (!gs::Settings::Get().keepPins) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        int best = -1;
        float bestD = 4.0f;   // two metres, squared
        for (int i = 0; i < g_count; ++i)
        {
            const float dx = g_pins[i].x - x, dz = g_pins[i].z - z;
            const float d = dx * dx + dz * dz;
            if (d <= bestD) { bestD = d; best = i; }
        }
        if (best < 0) return;
        for (int i = best; i + 1 < g_count; ++i) g_pins[i] = g_pins[i + 1];
        --g_count;
        Write();
        GS_LOG("[pins] taken out of the file; %d left", g_count);
    }

    int All(Saved* out, int n)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        int got = 0;
        for (int i = 0; i < g_count && got < n; ++i) out[got++] = g_pins[i];
        return got;
    }

    int Count()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_count;
    }
}
