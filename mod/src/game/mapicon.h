#pragma once
#include <cstdint>

// The map icon create call, slot 170 on the world map and minimap root controls.
//
// This build spies. It replaces slot 170 in both vtables with a function that
// records every argument the game passes and then forwards all fourteen of them
// to the original, unchanged. Nothing is created by us, nothing is altered on
// the way through. Once a session has captured what the game itself passes, the
// next build can replay a real call with the position swapped, which is a far
// safer first call than filling fourteen arguments from static analysis alone.
//
// The arguments. Eleven of them; the first spy session read thirteen because
// the dispatcher pushes seven registers where the constructor pushes five and
// the constructor's stack offsets were reused. Arguments 12 to 14 came back as
// return addresses, which is how that was caught.
//   1  this, the root control
//   2  const uint16_t*  icon type; 0 for the player, 0x020F for map points
//   3  const Key*       int64 id + uint8 kind; A0100001/0C is the player actor
//   4  const uint32_t*  the same id again for the player, 0 for map points
//   5  const float*
//   6  const float3*    the position
//   7  const char*      may be null
//   8  const char*      icon name, must be non-empty or the call does nothing
//   9  uint8_t          by value
//  10  const Struct36*  count at +4, pointer at +0x10; zeroed is safe
//  11  uint8_t          by value
// Three more are forwarded for safety and never read.

namespace gs::mapicon
{
    // A flat copy of one observed call. Plain data on purpose, so it can be
    // filled inside a __try frame.
    struct Capture
    {
        int surface = -1;          // 0 world map, 1 minimap
        uint64_t sequence = 0;
        void* self = nullptr;
        void* raw[14]{};
        uint16_t type = 0;
        int64_t keyId = 0;  uint8_t keyKind = 0;
        uint32_t dword4 = 0;
        float float5 = 0;
        float pos[3]{};
        char str7[48]{};
        char name8[64]{};
        uint8_t byte9 = 0;
        uint8_t struct10[36]{};
        uint8_t pointee[64]{};   // what struct10+0x10 points at, if anything
        bool pointeeOk = false;
        uint8_t byte11 = 0;
        bool str7Null = true;
        bool ok = false;          // every read succeeded
    };

    // Swap slot 170 on both vtables. Both must be RTTI-verified before this is
    // called. Returns how many of the two were installed.
    int InstallSpy(uintptr_t worldVtable, uintptr_t miniVtable);

    // And a spy on slot 171, the remove.
    //
    // The map's own delete key should take the mod's pins off, the way
    // it takes my own markers off. That cannot work while the mod's pins are
    // only icons: the game deletes what it knows about, and it does not know
    // about them.
    //
    // So the question is what the game does when I press delete, and the
    // cheapest way to find out is to watch. Slot 171 is where a removal has to
    // end up, whatever decided on it. One deletion of one of my own markers
    // prints the key the game used, the arguments beside it, and the stack
    // above it, which is the same trick that found the create.
    int InstallRemoveSpy(uintptr_t worldVtable, uintptr_t miniVtable);
    void RemoveSpy();

    // Counts and the most recent capture per surface, for the hotkey dry run.
    uint64_t Seen(int surface);
    bool Last(int surface, Capture& out);

    // Place a pin now. Must be called on the game's UI thread, which in practice
    // means from the tick. Uses the constants session ten proved: type 1, kind
    // 0x15, name MapIcon_Pin_Marker, and a key id of our own from 1001 up.
    // Returns what slot 170 returned.
    // `y` is the node's real height. Session ten's replay copied a captured
    // call whose height was zero and the mod kept passing zero ever since;
    // session forty-eight caught the game creating its own player marker at
    // (-9730.99, 562.29, -4303.08) while the player stood at (-9731.9,
    // 562.2, -4301.8), so the game gives its icons a real height and a pin
    // at zero is a pin the map has to guess at.
    // `keyId` is the icon's key when `haveKey`, and otherwise one of the mod's
    // own from 1001 up. It carries a real record's id when there is one, since
    // a key the game's marker list knows about is the only kind that can lead
    // anywhere. Zero is a real id, which is why the flag is separate.
    void* PlacePinNow(void* worldRoot, float x, float y, float z, const char* label,
                      int64_t keyId, bool haveKey);

    // True if the mod has already placed a pin within `radius` of (x, z).
    // The spec: one marker per area, never a second one on top of it.
    bool PinNear(float x, float z, float radius);

    // How many pins are still on the map carrying a key at or above `minId`,
    // which is how the mod counts the ones that have a record behind them.
    int LivePinsAtOrAbove(int64_t minId);

    // Every live pin's key, whatever it is, so a caller can take them all off.
    // Not filtered by id: a pin the mod drew before it could write a record
    // carries a key of its own invention, and that is exactly the pin that
    // gets left behind on the map when the real one replaces it.
    int LivePinKeys(int64_t* out, int n);

    // Record a place as taken.
    void Remember(float x, float y, float z, const char* label, int64_t keyId);

    // The game took a marker off the map. If it was one of the mod's, the mod
    // stops counting it, so the one-per-area rule lets that place be marked
    // again.
    // `x` and `z` come back with where it stood, so the caller can take it out
    // of the file as well.
    // `byGame` false is the mod taking its own pin off, which logs itself.
    bool Forget(int64_t keyId, float* x, float* z, bool byGame = true);

    // The mod's pins still on the map, with where each stands and its label.
    // Read on the game's UI thread, the one that places and forgets them.
    struct LivePin { int64_t id; float x, y, z; char label[16]; };
    int LivePins(LivePin* out, int n);

    // A world has been rebuilt, so nothing the mod drew is on the map any
    // more and none of its keys mean anything. Start again.
    void ForgetAll();

    // Take one of the mod's icons off the world map by its key. Used when
    // the map asks for a marker the mod owns to go and the mod does the
    // removal itself. Must be called on the game's UI thread.
    bool RemoveIcon(void* worldRoot, int64_t keyId);

    // The world map root the spy last saw a call on, or null. The tick uses
    // this when the scan has not located the object yet.
    void* LastWorldRoot();
}
