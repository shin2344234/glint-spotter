#pragma once
#include <cstdint>

// The pins, written down, so a save and a reload does not lose them.
//
// Nothing the mod does reaches the game's save file, which is deliberate: a
// pin can never be left behind in a save somebody keeps after uninstalling.
// The cost of that is that the game clears every pin when it loads, and I
// wants them back. So the mod keeps its own list beside the plugin and puts
// them on the map again when a world appears.
//
// One flat file, GlintSpotter.pins, one pin per line:
//
//   -9715.7 567.5 -4142.7 Glint
//
// Plain text on purpose. It can be read, edited and deleted by hand, and
// deleting it is how somebody clears every pin at once.
//
// It is one file for the whole game, not one per save. There is no save
// identity the mod can read yet, so a second character sees the first one's
// pins. That is worth knowing and not worth blocking the feature over.

namespace gs::pinstore
{
    struct Saved
    {
        float x = 0, y = 0, z = 0;
        char label[16]{};
    };

    // Read the file beside the plugin. Called once at startup.
    void Load(void* selfModule);

    // Remember a new pin and write the file. Ignored when the list is full.
    void Add(float x, float y, float z, const char* label);

    // Forget the pin nearest (x, z) within a couple of metres, and write the
    // file. Called when the map removes one.
    void Drop(float x, float z);

    // Copy the list out. Returns how many were written.
    int All(Saved* out, int n);
    int Count();
}
