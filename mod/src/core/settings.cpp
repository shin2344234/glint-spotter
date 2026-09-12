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

    // Controller buttons by name, joined with + or a comma. Case does not
    // matter and anything unrecognised is ignored, which is logged by the
    // caller when the whole string names nothing.
    uint16_t ChordBits(const char* s)
    {
        struct Named { const char* name; uint16_t bit; };
        static const Named kButtons[] = {
            {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
            {"START", 0x0010}, {"BACK", 0x0020}, {"LS", 0x0040}, {"RS", 0x0080},
            {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000},
            {"X", 0x4000}, {"Y", 0x8000},
        };
        uint16_t bits = 0;
        char word[16];
        size_t w = 0;
        for (const char* p = s;; ++p)
        {
            if (*p && *p != '+' && *p != ',' && *p != ' ')
            {
                if (w + 1 < sizeof(word)) word[w++] = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
                continue;
            }
            word[w] = 0;
            if (w)
                for (const Named& b : kButtons)
                    if (strcmp(word, b.name) == 0) { bits |= b.bit; break; }
            w = 0;
            if (!*p) break;
        }
        return bits;
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
        fputs("; What is worth a pin. Each entry is matched anywhere in the node's own\n", f);
        fputs("; prefab name, so 'clue' marks gimmick_item_graymane_clue_01. Add what you\n", f);
        fputs("; hunt for, remove what clutters your map. Names appear in the log beside\n", f);
        fputs("; every pin, so the list can be grown from a session.\n", f);
        fputs("Mark=clue,artifact,treasure,relic,chest,challenge,standstone,socket_collection,", f);
        fputs("gather,ore,herb,flower,mushroom,useartifact,puzzle_attach,dial,crank,lever\n", f);
        fputs("; An optional cap in metres on how far a node can be. Zero means\n", f);
        fputs("; everything the game has loaded around you, which is the natural limit.\n", f);
        fputs("; The controller buttons that place a mark, held together for Hold\n", f);
        fputs("; milliseconds. Names: A B X Y LB RB LS RS UP DOWN LEFT RIGHT BACK START.\n", f);
        fputs("Chord=RB+LB+A\n", f);
        fputs("Hold=0\n", f);
        fputs("; 1 also copies every pin onto the minimap. This crashed the game in\n", f);
        fputs("; session eighty-two, one frame after the call returned. Leave it off.\n", f);
        fputs("MiniPin=0\n", f);
        fputs("; 1 buzzes the controller when a pin lands.\n", f);
        fputs("Rumble=1\n", f);
        fputs("; How far either side of the sight line a press looks, in metres, the\n", f);
        fputs("; same at every distance. A press fires when you ask, so it does not\n", f);
        fputs("; need the slack that grows with range the automatic marker needs.\n", f);
        fputs("Rod=8.0\n", f);
        fputs("; How far out a press looks, in metres. Zero means no limit; the height\n", f);
        fputs("; test is what keeps distant things honest now.\n", f);
        fputs("PressReach=0\n", f);
        fputs("; 1 lets a press the table cannot answer guess a spot from terrain.\n", f);
        fputs("; Off, because past eighty metres that guess is an extrapolation.\n", f);
        fputs("RayFallback=0\n", f);
        fputs("; 1 hunts for live objects by scanning the heap. Eighteen seconds of\n", f);
        fputs("; stall and nothing the mod still needs; off.\n", f);
        fputs("Sweep=0\n", f);
        fputs("; 1 walks the heap for the player component instead of waiting for the\n", f);
        fputs("; actor manager. Ready sooner, at the cost of a freeze while it walks.\n", f);
        fputs("Scan=1\n", f);
        fputs("; 1 turns the investigation output back on: the table dump as it loads,\n", f);
        fputs("; the string catalogue, the entity listing, the per-field probe. It is\n", f);
        fputs("; a thousand lines a minute and it stutters the frame.\n", f);
        fputs("Verbose=0\n", f);
        fputs("; The same for the automatic glint marker. Wide on purpose: nobody is\n", f);
        fputs("; aiming carefully during a flash, and a miss there is a glint lost.\n", f);
        fputs("AutoCone=2.0\n", f);
        fputs("; A ceiling in metres on how far out a level gimmick may be and still\n", f);
        fputs("; count as the thing the crosshair is on. Zero means none, which put a\n", f);
        fputs("; pin three kilometres away once.\n", f);
        fputs("Reach=1500\n", f);
        fputs("Radius=0\n", f);
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
            else if (_stricmp(key, "Mark") == 0)
            {
                strncpy_s(g_values.mark, sizeof(g_values.mark), val, _TRUNCATE);
            }
            else if (_stricmp(key, "Kinds") == 0)
            {
                strncpy_s(g_values.kinds, sizeof(g_values.kinds), val, _TRUNCATE);
            }
            else if (_stricmp(key, "Guess") == 0)
            {
                g_values.guess = atoi(val) != 0;
            }
            else if (_stricmp(key, "Survey") == 0)
            {
                g_values.survey = atoi(val) != 0;
            }
            else if (_stricmp(key, "Chord") == 0)
            {
                const uint16_t bits = ChordBits(val);
                if (bits) g_values.chord = bits;
                else GS_LOG_ERR("settings: Chord=%s named no button I know, keeping the default", val);
            }
            else if (_stricmp(key, "Rod") == 0)
            {
                const float r = static_cast<float>(atof(val));
                if (r >= 0.05f && r <= 60.0f) g_values.rodMetres = r;
                else GS_LOG_ERR("settings: Rod=%s is out of range, keeping %.2f metres",
                                val, g_values.rodMetres);
            }
            else if (_stricmp(key, "PressReach") == 0)
            {
                const float r = static_cast<float>(atof(val));
                if (r == 0.0f || (r >= 50.0f && r <= 20000.0f)) g_values.pressReach = r;
                else GS_LOG_ERR("settings: PressReach=%s is out of range, keeping %.0f",
                                val, g_values.pressReach);
            }
            else if (_stricmp(key, "Verbose") == 0)
            {
                g_values.verbose = atoi(val) != 0;
            }
            else if (_stricmp(key, "Scan") == 0)
            {
                g_values.scan = atoi(val) != 0;
            }
            else if (_stricmp(key, "Sweep") == 0)
            {
                g_values.sweep = atoi(val) != 0;
            }
            else if (_stricmp(key, "RayFallback") == 0)
            {
                g_values.rayFallback = atoi(val) != 0;
            }
            else if (_stricmp(key, "Cone") == 0)
            {
                GS_LOG("settings: Cone is gone. A press uses Rod, in metres, the same at every "
                       "distance; AutoCone still governs the flash.");
            }
            else if (_stricmp(key, "AutoCone") == 0)
            {
                const float c = static_cast<float>(atof(val));
                if (c >= 0.05f && c <= 45.0f) g_values.autoConeDeg = c;
                else GS_LOG_ERR("settings: AutoCone=%s is out of range, keeping %.2f degrees",
                                val, g_values.autoConeDeg);
            }
            else if (_stricmp(key, "Rumble") == 0)
            {
                g_values.rumble = atoi(val) != 0;
            }
            else if (_stricmp(key, "MiniPin") == 0)
            {
                g_values.miniPin = atoi(val) != 0;
            }
            else if (_stricmp(key, "Hold") == 0)
            {
                const long h = atol(val);
                if (h >= 0 && h <= 5000) g_values.holdMs = static_cast<uint32_t>(h);
                else GS_LOG_ERR("settings: Hold=%s is out of range, keeping %lu", val,
                                static_cast<unsigned long>(g_values.holdMs));
            }
            else if (_stricmp(key, "Reach") == 0)
            {
                const float r = static_cast<float>(atof(val));
                if (r == 0.0f || (r >= 50.0f && r <= 20000.0f)) g_values.reach = r;
                else GS_LOG_ERR("settings: Reach=%s is out of range, keeping %.0f", val, g_values.reach);
            }
            else if (_stricmp(key, "Radius") == 0)
            {
                const float r = static_cast<float>(atof(val));
                if (r == 0.0f || (r >= 5.0f && r <= 2000.0f)) g_values.radius = r;
                else GS_LOG_ERR("settings: Radius=%s is out of range, keeping %.0f", val, g_values.radius);
            }
            else
            {
                GS_LOG("settings: unknown key '%s' ignored", key);
            }
        }
        fclose(f);

        if (g_values.radius > 0.0f)
            GS_LOG("settings: Key=%02X (%s), Spy=%d, Radius=%.0f metres", g_values.key, KeyName(g_values.key),
                   g_values.spy ? 1 : 0, g_values.radius);
        else
            GS_LOG("settings: Key=%02X (%s), Spy=%d, no radius cap: everything the game has loaded",
                   g_values.key, KeyName(g_values.key), g_values.spy ? 1 : 0);
        if (g_values.reach > 0.0f)
            GS_LOG("settings: Reach=%.0f metres, Chord=0x%04X held %lu ms", g_values.reach,
                   g_values.chord, static_cast<unsigned long>(g_values.holdMs));
        else
            GS_LOG("settings: no reach ceiling, Chord=0x%04X held %lu ms", g_values.chord,
                   static_cast<unsigned long>(g_values.holdMs));
        GS_LOG("settings: Rod=%.2f metres out to %.0f on a press, AutoCone=%.2f degrees "
               "on the flash", g_values.rodMetres, g_values.pressReach, g_values.autoConeDeg);
        GS_LOG("settings: Kinds=%s", g_values.kinds);
        GS_LOG("settings: Mark=%s", g_values.mark);
        return g_values;
    }

    const Values& Get() { return g_values; }

    bool Marked(const char* path)
    {
        if (!path || !path[0]) return false;
        const char* p = g_values.mark;
        while (*p)
        {
            while (*p == ' ' || *p == ',') ++p;
            const char* start = p;
            while (*p && *p != ',') ++p;
            size_t len = static_cast<size_t>(p - start);
            while (len && (start[len - 1] == ' ' || start[len - 1] == '\t')) --len;
            if (len == 0) continue;
            // A case insensitive search for this entry in the path.
            for (const char* at = path; *at; ++at)
                if (_strnicmp(at, start, len) == 0) return true;
        }
        return false;
    }

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
