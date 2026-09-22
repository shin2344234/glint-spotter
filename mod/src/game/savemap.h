#pragma once
#include <cstdint>

// Which level placements the save says have been taken, read live.
//
// The level gimmick table lists every placement for good, collected or not,
// so the flash could pin a sealed artifact taken an hour earlier. The save
// knows which ones are gone: a load does not spawn them again.
//
// It keeps that in FieldGimmickSaveData (vtable 0x058AFFC8 on 2949, 0x3D8
// bytes, the records of a vector inside FieldSaveData, vtable 0x058B0A48).
// Each record carries the gimmick's origin transform, position at +0x210,
// and its saved state as a hash at +0x21C, _initStateNameHash. State hashes
// are Jenkins lookup3 of the state's name (Master Looter's statehash.py).
// item_sealed_artifact.binarygimmick declares Wait, GimmickOn, Lock,
// Deactive and Clear, and a collection gimmick goes to Clear when it is
// taken. Clear hashes to 0xE300ACFE, the value a record dumped on 22
// September held at +0x21C.
//
// So a placement is taken when a record within three metres of it is in
// Clear. Having a record is not enough: an unfinished standstone challenge
// has one too.
//
// The records are reached with no search. The player's server
// ServerContentsMiscActorComponent loads each FieldSaveData into a map at
// +0x2C0 (its slot 10, 0x028B72F0, inserting with 0x028EEC50), and the
// component is found from the server actor manager's global. The route and
// the map's layout are spelt out in savemap.cpp.
//
// The dead ends that led here are in private/re-glint-2944.md: the reflected
// map state classes are not resident, and the map state list on the player's
// server ServerContentsMiscActorComponent stays empty.
namespace gs::savemap
{
    // Starts the reader's thread the first time it is called. Cheap after.
    void Start();

    // Ends the reader's thread and, unless the process is going, waits for
    // it. Called from Mod::Shutdown.
    void Stop(bool processTerminating);

    // Is the placement at this address taken, by the save? False until the
    // records have been read.
    bool Completed(uintptr_t placementAt);

    // How many placements the save has as taken, for the log.
    int TakenCount();

    // The watch's save mode reads this; nothing to watch here.
    uintptr_t Component();
}
