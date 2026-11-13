#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Which of the mod's pins have done their job, worked out off the game's
// thread. Through 1.1.40 this ran on the minimap tick once a second, and on 13
// November the tick's share of it took 3.0 to 5.9 ms twenty times in one
// session. The pins and the taken set were small; the time went on reading the
// player (a VirtualQuery per read, four more for the actor's class name, and a
// camera read every half second) and on waiting for locks the save reader
// holds. The key thread now decides, and the tick only takes off what it is
// handed.
namespace gs::clearcheck
{
    // Places on a 16 metre grid, so "is there one within three metres" looks
    // at nine cells instead of every place. The save's taken set is a few
    // dozen on a new save and can reach the 45,604 placements the 2.05.00
    // table holds.
    class SpotGrid
    {
    public:
        void Clear();
        void Add(float x, float y, float z);
        // Within `across` metres on the ground and `up` metres up or down.
        // `across` has to be under the cell size.
        bool Near(float x, float y, float z, float across, float up) const;
        size_t Size() const { return n_; }

    private:
        struct Spot { float x, y, z; };
        static int64_t Cell(float x, float z);
        std::unordered_map<int64_t, std::vector<Spot>> cells_;
        size_t n_ = 0;
    };

    struct Pin
    {
        int64_t id = 0;
        float x = 0, y = 0, z = 0;
        char label[16]{};
    };

    struct Done
    {
        Pin pin;
        char why[64]{};
    };

    struct Inputs
    {
        bool clearTaken = false;
        float clearNear = 0.0f;   // metres; 0 leaves key and chord pins alone
        float px = 0.0f, pz = 0.0f;
    };

    // Whether the placement under a Glint pin has gone, from the save reader's
    // taken set or the pin file's taken lines.
    using TakenFn = bool (*)(float x, float y, float z);

    // A key or chord pin arms once the player is this far past ClearNear.
    constexpr float kArmMetres = 10.0f;

    // The pins that should come off, written to `out`. A Glint pin goes when
    // `taken` says so. Any other label goes when the player walks within
    // ClearNear of it after having been well away; `armed` keeps that per key
    // and belongs to the caller's thread. Returns how many were written.
    int Decide(const Pin* pins, int n, const Inputs& in, TakenFn taken,
               std::unordered_map<int64_t, bool>& armed, Done* out, int outMax);
}
