#pragma once
#include <cstdint>

// Which save is being played.
//
// The game keeps its saves at
//
//   %LOCALAPPDATA%\Pearl Abyss\CD\save\<account>\slot<N>\save.save
//
// with a second file, lobby.save, holding whatever the load menu needs to draw
// a row. The account is the Steam account id, which the game itself writes
// into steam_autocloud.vdf beside the slots.
//
// Nothing in the game hands the mod a slot number, and the save payload is
// encrypted, so the only honest way to learn which save the player is in is to
// watch the file being opened. That is what this does: the two file-open
// imports are pointed at a detour that looks at the path and gets out of the
// way. Reading save.save is a load. Writing it is a save.
//
// lobby.save is deliberately ignored. The load menu reads every slot's lobby
// file to draw the list, so treating those as loads would say the player is in
// whichever save they scrolled past last.

namespace gs::saveslot
{
    struct Id
    {
        uint32_t account = 0;
        int32_t slot = -1;

        bool ok() const { return account != 0 && slot >= 0; }
        bool operator==(const Id& o) const { return account == o.account && slot == o.slot; }
    };

    struct Event
    {
        Id id;
        bool write = false;   // false: the game read this save, so it loaded it
    };

    // Point the file-open imports at the detour. False when the game does not
    // import them, in which case pins stay in one set for the whole game.
    bool Install();

    // Put the imports back, for the unload that leaves the process running.
    void Remove();

    // Everything seen since the last call, oldest first. Called from the tick.
    int Take(Event* out, int n);

    // "slot100 of account 8f3a", for the log. The account is folded to four
    // hex digits on purpose: logs get posted on the internet and a Steam
    // account id is the number in the middle of somebody's profile URL.
    void Text(const Id& id, char* out, int n);
}
