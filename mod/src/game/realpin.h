#pragma once
#include <cstdint>

// The game's own "place a marker here" call.
//
// Every pin this mod has ever drawn was a picture. It called the map's icon
// dispatcher directly, which puts a marker on the screen and nothing in the
// game's data, so the delete button had nothing to act on and Seth has been
// stuck with his pins all session. This is the other end of the same feature:
// the function the game itself runs when the player places a marker by hand.
//
// It was found by following the frames the spy captured around a marker Seth
// placed and deleted in session a hundred and two, then reading the code they
// named. The chain, all of it disassembled rather than guessed:
//
//   the map's delete sends a remove request over an in-process wire
//   the request's own handler calls 0x27FE600(submodule, id, second)
//   placing a marker calls 0x27FE130(submodule, unused, &pos, &b1, &b2, second)
//
// and both reach the list the same way the network acknowledgement does,
// through *(*(actor + 0x68) + 0x168). Client and server are the same object
// here, which is the whole reason this works: a record written by the create
// call is a record the delete call can find.
//
// 0x27FE130 does all of it. It range checks the two style bytes, picks the
// list (0 for a marker, 1 for a traced one), evicts the oldest when the list
// is full, assigns an id, appends the twenty-four byte record and publishes
// the change, which is what makes the icon appear. Nothing about it needs the
// wire.
//
// The signature, read off the prologue and both branches:
//
//   void (*)(void* submodule,      // *(*(actor + 0x68) + 0x168)
//            void* unused,         // spilled and overwritten, never read
//            const float* pos,     // x, y, z; the game passes height zero
//            const uint8_t* b1,    // style byte, below 0x0E
//            const uint8_t* b2,    // style byte, below 0x0F
//            uint8_t second)       // 0 for a marker, 1 for a traced marker
//
// The cap is the game's, not ours. With the online flag clear it keeps fifteen
// and drops the oldest to make room; with it set the list holds five hundred.
// Either way the mod does not choose, which is the point: these are the
// player's markers, and they behave like it.

namespace gs::realpin
{
    // Whether the call can be made right now. `why` is filled with the reason
    // when it cannot, and is safe to log every press.
    bool Ready(const char** why);

    // Place one. Returns the id the game gave the record, or 0 when the record
    // did not appear. The height is not passed on: the game writes zero for
    // its own markers and the icon is drawn from this record.
    int64_t Place(float x, float z);

    // How many markers the game is holding, or -1 when the list cannot be read.
    int Count();

    // The list, both kinds, and the flag that decides the cap.
    void LogState(const char* why);
}
