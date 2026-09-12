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
//
// Opening save.save is not enough either. The first session with this watch
// caught the game opening all eight of them a moment before a world appeared,
// which is a look at each rather than a load of one. So the bytes are counted:
// a handle that has read most of its file is a save being loaded, and a handle
// that read a header is a save being looked at. The look is still reported, as
// the answer of last resort for a build where the counting sees nothing.

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
        bool write = false;   // the game is saving into this slot
        bool full = false;    // it read the whole file, so it is loading it
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
