#include "core/pinstore.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/settings.h"

namespace
{
    constexpr int kMaxPins = 256;     // per save
    constexpr size_t kMaxGroups = 32; // saves remembered before the oldest goes

    struct Slot
    {
        uint32_t account = 0;
        int32_t slot = -1;
        bool operator==(const Slot& o) const
        {
            return account == o.account && slot == o.slot;
        }
    };

    struct Group
    {
        std::vector<Slot> slots;      // empty means it belongs to no save yet
        std::vector<gs::pinstore::Saved> pins;
        uint32_t touched = 0;         // for deciding which one to forget
    };

    std::mutex g_mutex;
    std::vector<Group> g_groups;
    size_t g_cur = 0;
    uint32_t g_clock = 0;
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

    // Everything below is called with the lock held.

    Group& Cur()
    {
        if (g_groups.empty()) g_groups.emplace_back();
        if (g_cur >= g_groups.size()) g_cur = g_groups.size() - 1;
        return g_groups[g_cur];
    }

    void Write()
    {
        if (g_path.empty()) return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_path.c_str(), L"w") != 0 || !f) return;
        fputs("; Glint Spotter's pins. One per line: x y z label.\n", f);
        fputs("; Each group belongs to one save. The save lines under a header say which\n", f);
        fputs("; slots that is, as account/slot. Delete a group to clear that save's pins,\n", f);
        fputs("; or delete the file to clear all of them.\n", f);
        for (size_t i = 0; i < g_groups.size(); ++i)
        {
            const Group& g = g_groups[i];
            if (g.pins.empty() && g.slots.empty()) continue;
            fprintf(f, "\n[%u]\n", static_cast<unsigned>(i + 1));
            for (const Slot& s : g.slots)
                fprintf(f, "save %u/%d\n", s.account, s.slot);
            for (const gs::pinstore::Saved& p : g.pins)
                fprintf(f, "%.3f %.3f %.3f %s\n", p.x, p.y, p.z, p.label);
        }
        fclose(f);
    }

    size_t Owner(const Slot& want)
    {
        for (size_t i = 0; i < g_groups.size(); ++i)
            for (const Slot& s : g_groups[i].slots)
                if (s == want) return i;
        return static_cast<size_t>(-1);
    }

    // A save nobody has claimed needs somewhere to live. Reuse the group in
    // hand when it has no save of its own, which is what carries the pins from
    // before this feature existed, and the ones dropped during a new game, on
    // to the save they turn out to belong to.
    size_t Mint(bool prune = true)
    {
        if (prune && g_groups.size() >= kMaxGroups)
        {
            // Never the one in hand, however long ago it was played. Erasing
            // that one would leave the index pointing at somebody else's pins.
            size_t oldest = g_groups.size();
            for (size_t i = 0; i < g_groups.size(); ++i)
            {
                if (i == g_cur) continue;
                if (oldest == g_groups.size() || g_groups[i].touched < g_groups[oldest].touched)
                    oldest = i;
            }
            if (oldest == g_groups.size())
            {
                g_groups.emplace_back();
                return g_groups.size() - 1;
            }
            GS_LOG("[pins] %u saves is as many as the file keeps; the least recently played one "
                   "and its %u pin(s) are forgotten", static_cast<unsigned>(kMaxGroups),
                   static_cast<unsigned>(g_groups[oldest].pins.size()));
            g_groups.erase(g_groups.begin() + static_cast<long long>(oldest));
            if (g_cur > oldest && g_cur) --g_cur;
        }
        g_groups.emplace_back();
        return g_groups.size() - 1;
    }

    void Bind(size_t group, const Slot& s)
    {
        const size_t was = Owner(s);
        if (was == group) return;
        if (was != static_cast<size_t>(-1))
        {
            std::vector<Slot>& list = g_groups[was].slots;
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i] == s) { list.erase(list.begin() + static_cast<long long>(i)); break; }
        }
        g_groups[group].slots.push_back(s);
    }
}

namespace gs::pinstore
{
    void Load(void* selfModule)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_path = PathFor(static_cast<HMODULE>(selfModule));
        g_groups.clear();
        g_groups.emplace_back();
        g_cur = 0;
        if (g_path.empty()) return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_path.c_str(), L"r") != 0 || !f)
        {
            GS_LOG("[pins] no pin file yet; one is written the first time you mark something");
            return;
        }

        size_t into = 0;   // the group being read, starting with the loose one
        int pins = 0;
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            if (line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
            if (line[0] == '[')
            {
                // Nothing is forgotten while reading. The file only ever holds
                // as many saves as the cap allows, and a hand-edited one that
                // holds more should lose the extra on the next write, not on
                // the way in.
                into = Mint(false);
                continue;
            }
            unsigned account = 0;
            int slot = 0;
            if (sscanf_s(line, "save %u/%d", &account, &slot) == 2 && account)
            {
                Slot s{account, slot};
                if (Owner(s) == static_cast<size_t>(-1)) g_groups[into].slots.push_back(s);
                continue;
            }
            if (g_groups[into].pins.size() >= static_cast<size_t>(kMaxPins)) continue;
            Saved p{};
            char label[64]{};
            const int n = sscanf_s(line, "%f %f %f %63s", &p.x, &p.y, &p.z, label,
                                   static_cast<unsigned>(sizeof(label)));
            if (n < 3) continue;
            strncpy_s(p.label, sizeof(p.label), n >= 4 ? label : "Pin", _TRUNCATE);
            g_groups[into].pins.push_back(p);
            ++pins;
        }
        fclose(f);

        // Start in a group that belongs to no save, so a pin dropped before
        // the game has loaded anything has somewhere to go, and so the pins
        // written before this feature existed are still waiting to be adopted
        // however many launches later. The one with pins in it wins: an empty
        // unattached group is just a spare, and picking the spare is what
        // stranded the first session's pins in a group nothing could reach.
        g_cur = g_groups.size();
        for (size_t i = 0; i < g_groups.size(); ++i)
            if (g_groups[i].slots.empty() && !g_groups[i].pins.empty()) { g_cur = i; break; }
        if (g_cur == g_groups.size())
            for (size_t i = 0; i < g_groups.size(); ++i)
                if (g_groups[i].slots.empty()) { g_cur = i; break; }
        if (g_cur == g_groups.size()) g_groups.emplace_back();

        const size_t loose = g_groups[g_cur].pins.size();
        int saves = 0;
        for (const Group& g : g_groups)
            if (!g.slots.empty()) ++saves;
        GS_LOG_OK("[pins] %d pin(s) read from the file beside the plugin, across %d save(s). "
                  "They go back on the map when the save they belong to is loaded.", pins, saves);
        if (loose)
            GS_LOG("[pins] %u of them are not attached to a save yet, from before the mod could "
                   "tell saves apart; the first save loaded takes them",
                   static_cast<unsigned>(loose));
    }

    void Loaded(uint32_t account, int32_t slot)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const Slot s{account, slot};
        const size_t owner = Owner(s);
        if (owner != static_cast<size_t>(-1))
        {
            g_cur = owner;
            g_groups[g_cur].touched = ++g_clock;
            GS_LOG_OK("[pins] this save has %u pin(s) of its own",
                      static_cast<unsigned>(g_groups[g_cur].pins.size()));
            return;
        }

        // Not seen before. The pins in hand come with it when they belong to
        // nothing else, which is how the old one-file-for-everything list
        // finds a home, and how a new game's pins catch up with its first
        // save.
        if (!Cur().slots.empty()) g_cur = Mint();
        const size_t adopted = g_groups[g_cur].pins.size();
        Bind(g_cur, s);
        g_groups[g_cur].touched = ++g_clock;
        Write();
        if (adopted)
            GS_LOG_OK("[pins] a save the mod has not seen before; the %u pin(s) in hand belong to "
                      "it from now on", static_cast<unsigned>(adopted));
        else
            GS_LOG_OK("[pins] a save the mod has not seen before, starting with no pins");
    }

    void SavedTo(uint32_t account, int32_t slot)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const Slot s{account, slot};
        Cur();
        if (Owner(s) == g_cur) return;
        Bind(g_cur, s);
        g_groups[g_cur].touched = ++g_clock;
        Write();
        GS_LOG_OK("[pins] saved into a slot this playthrough had not used; its %u pin(s) follow "
                  "it there", static_cast<unsigned>(g_groups[g_cur].pins.size()));
    }

    void NewGame()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Group& cur = Cur();
        if (cur.slots.empty() && cur.pins.empty()) return;
        g_cur = Mint();
        GS_LOG("[pins] a world with no save read in front of it, so this is a new game; it starts "
               "with no pins and takes them along when it is first saved");
    }

    void Add(float x, float y, float z, const char* label)
    {
        if (!gs::Settings::Get().keepPins) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        Group& g = Cur();
        if (g.pins.size() >= static_cast<size_t>(kMaxPins))
        {
            GS_LOG_ERR("[pins] this save already has %d pins written down; this one is not",
                       kMaxPins);
            return;
        }
        Saved s{};
        s.x = x; s.y = y; s.z = z;
        strncpy_s(s.label, sizeof(s.label), label && label[0] ? label : "Pin", _TRUNCATE);
        g.pins.push_back(s);
        g.touched = ++g_clock;
        Write();
    }

    void Drop(float x, float z)
    {
        if (!gs::Settings::Get().keepPins) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        Group& g = Cur();
        size_t best = static_cast<size_t>(-1);
        float bestD = 4.0f;   // two metres, squared
        for (size_t i = 0; i < g.pins.size(); ++i)
        {
            const float dx = g.pins[i].x - x, dz = g.pins[i].z - z;
            const float d = dx * dx + dz * dz;
            if (d <= bestD) { bestD = d; best = i; }
        }
        if (best == static_cast<size_t>(-1)) return;
        g.pins.erase(g.pins.begin() + static_cast<long long>(best));
        g.touched = ++g_clock;
        Write();
        GS_LOG("[pins] taken out of the file; %u left for this save",
               static_cast<unsigned>(g.pins.size()));
    }

    int All(Saved* out, int n)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const Group& g = Cur();
        int got = 0;
        for (size_t i = 0; i < g.pins.size() && got < n; ++i) out[got++] = g.pins[i];
        return got;
    }

    int Count()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return static_cast<int>(Cur().pins.size());
    }
}
