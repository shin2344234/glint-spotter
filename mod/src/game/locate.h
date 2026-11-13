#pragma once
#include <cstdint>

// Finding the fixed addresses again in whatever build is running.
//
// Two globals and five pieces of the marker code were plain RVAs out of
// signatures.h with nothing behind them, so every game patch turned the flash
// check, the level gimmick table and the map pins off until a new build came
// out. Each one has a route through RTTI that does not depend on the address:
//
//   the level gimmick and special mode managers are stored by slot 2 of their
//   own manager classes, mov qword ptr [rip + X], rcx ; ret;
//
//   the marker create and server remove are the one direct call from slot 2
//   of the create and remove request messages whose first bytes match the
//   recorded prologue, and the client copy's writer and eraser the same from
//   the two acknowledgement messages;
//
//   the online flag is the one cmp byte ptr [rip + X], imm8 in the create.
//
// The functions keep their recorded RVA when its prologue still matches, so a
// build that has not moved costs nothing. The globals have no such check, so
// the store is read every time and the recorded value is the fallback.
//
// All values are RVAs, 0 when neither route found one.

namespace gs::locate
{
    // One RTTI pass over the image for all six classes, then the reads.
    // Idempotent and thread safe; whichever thread asks first pays for it.
    void Resolve();

    uintptr_t LgsoGlobal();
    uintptr_t SpecialModeGlobal();
    uintptr_t PinCreate();
    uintptr_t PinServerRemove();
    uintptr_t PinUpsert();
    uintptr_t PinRemove();
    uintptr_t PinOnlineFlag();

    // Writes one log line per address, saying how it was found, and adds to
    // the startup pass's tally: as recorded, found again, or missing. Names
    // of the missing ones go into names[] while there is room.
    void Report(int& ok, int& stale, int& missing, const char** names, int cap);
}
