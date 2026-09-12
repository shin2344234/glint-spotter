#pragma once
#include <cstdint>

// The pins, written down, so a save and a reload does not lose them.
//
// Nothing the mod does reaches the game's save file, which is deliberate: a
// pin can never be left behind in a save somebody keeps after uninstalling.
// The cost of that is that the game clears every pin when it loads, and I want
// them back. So the mod keeps its own list beside the plugin and puts them on
// the map again when a world appears.
//
// The list is kept in groups, one per save, because a pin dropped in one
// playthrough has no business appearing in another. Which save is being played
// comes from gs::saveslot, which watches the game open the file. A group
// remembers every slot it has been written to, so saving into a fresh slot
// carries the pins along instead of starting empty: the game makes a new slot
// folder for every manual save, and pins keyed to the slot alone would be lost
// on the first one.
//
// GlintSpotter.pins, plain text on purpose, readable and editable by hand:
//
//   [1]
//   save 12800898/100
//   -9715.7 567.5 -4142.7 Glint
//
// Pins written before the group came along sit above the first [n] header, and
// the first save the mod recognises adopts them.

namespace gs::pinstore
{
    struct Saved
    {
        float x = 0, y = 0, z = 0;
        char label[16]{};
    };

    // Read the file beside the plugin. Called once at startup.
    void Load(void* selfModule);

    // The game read this save, so it is the one being played from now on.
    void Loaded(uint32_t account, int32_t slot);

    // The game wrote this save, so the slot belongs to whatever is being
    // played, whatever was in it before.
    void SavedTo(uint32_t account, int32_t slot);

    // A world was built with no save read in front of it, which is a new game.
    // Pins from here belong to nothing until the first save is written.
    void NewGame();

    // Whether the pins in hand are attached to a save yet.
    bool Bound();

    // Remember a new pin and write the file. Ignored when the list is full.
    void Add(float x, float y, float z, const char* label);

    // Forget the pin nearest (x, z) within a couple of metres, and write the
    // file. Called when the map removes one.
    void Drop(float x, float z);

    // Copy out the pins for the save being played. Returns how many.
    int All(Saved* out, int n);
    int Count();
}
