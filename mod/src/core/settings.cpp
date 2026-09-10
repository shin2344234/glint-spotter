#include "core/settings.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"

namespace
{
    gs::Settings::Values g_values;

    std::wstring IniPathFor(HMODULE self)
    {
        wchar_t buf[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return L"";
        std::wstring p(buf, n);
        const size_t dot = p.find_last_of(L'.');
        const size_t slash = p.find_last_of(L"\\/");
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) p.resize(dot);
        return p + L".ini";
    }

    void Trim(char* s)
    {
        char* e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
        char* b = s;
        while (*b == ' ' || *b == '\t') ++b;
        if (b != s) memmove(s, b, strlen(b) + 1);
    }

    void WriteDefaults(const std::wstring& path)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) return;
        fputs("[GlintSpotter]\n", f);
        fputs("; Trigger key as a hex virtual-key code. 91 is Scroll Lock, 7B is F12,\n", f);
        fputs("; 13 is Pause. Scroll Lock is the default because nothing else on this\n", f);
        fputs("; machine binds it and it has no side effect of its own.\n", f);
        fputs("Key=91\n", f);
        fputs("; 1 logs every map icon the game creates. 0 leaves the vtable alone.\n", f);
        fputs("Spy=1\n", f);
        fclose(f);
    }
}

namespace gs::Settings
{
    const Values& Load(void* selfModule)
    {
        g_values = Values{};
        const std::wstring path = IniPathFor(static_cast<HMODULE>(selfModule));
        if (path.empty()) return g_values;

        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f)
        {
            WriteDefaults(path);
            GS_LOG("settings: no ini, wrote defaults (Key=%02X %s, Spy=1)",
                   g_values.key, KeyName(g_values.key));
            return g_values;
        }

        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            Trim(line);
            if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char* key = line;
            char* val = eq + 1;
            Trim(key);
            Trim(val);

            if (_stricmp(key, "Key") == 0)
            {
                const unsigned long v = strtoul(val, nullptr, 16);
                if (v > 0 && v < 256) g_values.key = static_cast<uint32_t>(v);
                else GS_LOG_ERR("settings: Key=%s is not a hex VK code, keeping %02X", val, g_values.key);
            }
            else if (_stricmp(key, "Spy") == 0)
            {
                g_values.spy = atoi(val) != 0;
            }
            else
            {
                GS_LOG("settings: unknown key '%s' ignored", key);
            }
        }
        fclose(f);

        GS_LOG("settings: Key=%02X (%s), Spy=%d", g_values.key, KeyName(g_values.key), g_values.spy ? 1 : 0);
        return g_values;
    }

    const Values& Get() { return g_values; }

    const char* KeyName(uint32_t vk)
    {
        switch (vk)
        {
        case 0x13: return "Pause";
        case 0x2D: return "Insert";
        case 0x2E: return "Delete";
        case 0x91: return "Scroll Lock";
        case 0x90: return "Num Lock";
        default:
            if (vk >= 0x70 && vk <= 0x7B)
            {
                static char fkey[8];
                sprintf_s(fkey, "F%u", vk - 0x70 + 1);
                return fkey;
            }
            return "key";
        }
    }
}
