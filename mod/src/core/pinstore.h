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
    // Pins one save keeps. The restore queue in tick.cpp is sized from this, so
    // every pin the file holds for a save can go back on the map. Through 1.1.40
    // that queue held 32 and dropped the rest without a word, which was
    // KarlUrbanJeung's bug 1172040 on 12 November.
    constexpr int kMaxPinsPerSave = 256;

    // Two pins closer than this across, and within kSameSpotUp up or down, are
    // the same pin. Marking a glint again gives its table position to the
    // millimetre, so the duplicates bug 1172040 wrote sit at exactly the same
    // place; half a metre leaves room for rounding in the file and none for two
    // real pins, which the map keeps four metres apart anyway.
    constexpr float kSameSpot = 0.5f;
    constexpr float kSameSpotUp = 2.0f;

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

    // Remember a new pin and write the file. Ignored when the list is full, and
    // when this save already has a pin at the same spot, which is logged.
    // Returns true when the pin is in the file afterwards, either way.
    bool Add(float x, float y, float z, const char* label);

    // Forget the pin nearest (x, z) within a couple of metres, and write the
    // file. Called when the map removes one.
    void Drop(float x, float z);

    // A placement this save has picked up that the save itself never marks as
    // taken, a sealed artifact being the one found so far. Held in memory
    // until the game writes the save, then written as a "taken x y z" line in
    // the save's group, which a build from before this skips as a line it
    // cannot read. A load before that forgets it, since the save loaded does
    // not have the pick up either.
    void AddTaken(float x, float y, float z);

    // Whether this save has one of those within three metres across and six
    // up or down.
    bool TakenNear(float x, float y, float z);

    // Copy out the pins for the save being played. Returns how many.
    int All(Saved* out, int n);
    int Count();

    // Pins written down under every save but the one being played. A load
    // that brings back nothing while this is above zero is the case the log
    // has to explain.
    int Elsewhere();
}
