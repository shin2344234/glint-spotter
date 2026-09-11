#pragma once
#include <cstdint>

// Every notable level gimmick in the world, with its position, read from the
// game's own database.
//
// This is the source the feature needed and spent sixty-six sessions not
// having. The actor manager only carries objects the player has walked up to,
// so at the range a glint is visible there is nothing to pick. This table does
// not care: 17,728 placements across 171 records, covering the whole map, read
// from a fixed global with no scanning.
//
// HOW IT IS REACHED
//
// pa::LevelGimmickSceneObjectInfoManager keeps its own pointer in a module
// global. Slots 2 and 3 of its vtable are the setter and the clear, and both
// write the same address, RVA 0x06C328C0. The lookup at RVA 0x00433370 reads
// the count from manager+0x08 and an array of record pointers from
// manager+0x58 and indexes it by a plain u32.
//
// A record is 0x70 bytes and owns lists in the usual {pointer, count,
// capacity} shape. A list's elements are 0xC8 bytes each and carry a
// forty-byte Transform at +0x64: quaternion, then position, then scale. That
// is the same Transform the .palevel files on disk carry, which is two
// independent sources agreeing on one layout.
//
// WHY IT IS THE RIGHT TABLE
//
// Session sixty-seven, with the player at (-9706.5, 559.9, -4235.0), found
// record 13 element 78 at (-9715.7, 567.5, -4142.7). Seth's own map marker on
// the glint he has been testing against since session forty-seven sits at
// (-9714.073, -4141.196). Those are 2.2 metres apart. The thing he is aiming
// at is in here.
//
// The records also carry names, read as engine strings through a pointer near
// the head of an element: "Mission_PororinVillage_Bell_All_Calphade",
// "Hernand_Bell". Notable gimmicks, the kind that earn a map icon, which is
// why a bell and a glint are both in the same table.

namespace gs::lgso
{
    struct Place
    {
        float x = 0, y = 0, z = 0;
        uint16_t record = 0;
        uint16_t element = 0;
    };

    // Read the table. Cheap to call again: it returns what it has unless the
    // manager pointer has changed, which happens on a level transition.
    // Returns how many placements are held.
    int Load();

    int Count();

    // The placements nearest the given bearing, smallest angle first.
    // `px, pz` is the player, `ox, oz` the eye, `ux, uz` the unit view bearing.
    // Anything closer than `minFromPlayer` or beyond `maxRange` is skipped, and
    // so is anything whose bearing error exceeds `maxAngle` radians.
    int OnBearing(float px, float pz, float ox, float oz, float ux, float uz,
                  float maxAngle, float minFromPlayer, float maxRange,
                  Place* out, float* angles, int n);

    // The placements nearest a point, for the log.
    int Near(float px, float pz, Place* out, int n);
}
