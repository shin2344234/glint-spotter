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
// capacity} shape. A list's elements are 0xD8 bytes each (0xC8 on 2850) and carry a
// forty-byte Transform at +0x74 (+0x64 on 2850): quaternion, then position, then scale. That
// is the same Transform the .palevel files on disk carry, which is two
// independent sources agreeing on one layout.
//
// WHY IT IS THE RIGHT TABLE
//
// Session sixty-seven, with the player at (-9706.5, 559.9, -4235.0), found
// record 13 element 78 at (-9715.7, 567.5, -4142.7). My own map marker on
// the glint I have been testing against since session forty-seven sits at
// (-9714.073, -4141.196). Those are 2.2 metres apart. The thing I am aiming
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
        char name[56]{};   // the element's own name, empty when it has none
    };

    // Every distinct name in the table, with how many placements carry it.
    // Printed once so the log says what kinds are in there and a filter can be
    // written against real names rather than guesses.
    void LogKinds();

    // Read the table. Cheap to call again: it returns what it has unless the
    // manager pointer has changed, which happens on a level transition.
    // Returns how many placements are held.
    int Load();

    int Count();

    // The placements the crosshair is on, nearest the player first.
    //
    // By how far each one sits from the ray, not by its angle. Over a complete
    // table an angle is the wrong measure: a placement five hundred metres out
    // subtends almost nothing, so it wins the smallest-angle test against the
    // thing the player is actually looking at every time. Session sixty-eight
    // is that mistake in the log, picking objects at 497, 700 and 869 metres
    // all at 0.0 degrees off while the glint sat at a hundred and nineteen.
    //
    // Distance from the ray is the honest measure and it does not care about
    // range. Among everything within `maxPerp` metres of the line, the nearest
    // along it wins, which is what "what am I pointing at" means.
    //
    // The cone is a floor and a slope, because one number cannot serve both
    // callers. A placement counts when it is within `maxPerp` metres of the
    // line or `perpFrac` of its own range, whichever is larger, to a ceiling
    // of thirty.
    //
    // Session seventy-eight is why they are separate. The automatic glint
    // search wants eight metres flat, because a crosshair sways while flying
    // and the thing it is looking for is far enough that eight metres is a
    // couple of degrees. A deliberate press wants the opposite: at nineteen
    // metres a floor of eight is a cone twenty-four degrees wide, so the press
    // was picking whatever a dense sector record happened to have off to one
    // side. Two metres and one and a half per cent is under a degree from
    // fifty metres out and still forgiving up close.
    //
    // `px, pz` is the player, `ox, oz` the eye, `ux, uz` the unit view bearing.
    // `dists` receives each kept placement's distance along the ray.
    //
    // `anyKind` turns the Kinds filter off. Glint hunting wants the filter:
    // the flash names one thing and a bonfire is not it. A deliberate press
    // wants the opposite, because the player is pointing at a ruin or a camp
    // or a bridge and asking for that, and the table holds all of it.
    // `oy` is the eye's height and `slope` the view's rise per metre of
    // ground covered, so the sight line's height at a placement's range is
    // `oy + slope * along`. A placement further than `maxVert` from that is
    // refused however well it lines up on the map.
    //
    // This was missing for forty-five builds and it is why every aperture
    // failed. The search was two dimensional over a three dimensional world,
    // so a bridge twelve hundred metres above my head counted as being on
    // the crosshair because its shadow was.
    int OnBearing(float px, float pz, float ox, float oy, float oz,
                  float ux, float uz, float slope,
                  float maxPerp, float perpFrac, float maxVert,
                  float minFromPlayer, float maxRange,
                  Place* out, float* dists, int n, bool anyKind = false);

    // The single placement closest to the view line, whatever it is called
    // and however far off the line it sits.
    //
    // OnBearing answers "what is the crosshair on" and refuses everything
    // else, which is right for placing a pin and useless for explaining why
    // no pin was placed. I aimed at a glint, got nothing, and the log had
    // nothing to say about it: no line for a table that found nothing, and
    // the only message on offer talked about the old actor search. This is
    // the near miss, so the next time it happens the log can say whether the
    // aim was off, the filter refused it, or the table really has nothing
    // there.
    //
    // Returns false when the table is empty. `perp` is metres off the line,
    // `along` metres down it, `refused` whether the Kinds list turned it away.
    bool NearestToLine(float ox, float oz, float ux, float uz, float maxRange,
                       Place* out, float* along, float* perp, bool* refused);

    // The placements closest to the sight line, by how far off it they sit,
    // ignoring every filter and every aperture.
    //
    // This exists because five apertures have now been tried and none of them
    // answered the question underneath: is the thing being pointed at even
    // in this table? If it is, it turns up here with a small perpendicular
    // distance and the aperture is what needs changing. If the nearest thing
    // to the sight line is twenty metres off it, no aperture will ever find
    // it and the table simply does not hold what you are looking at.
    //
    // `alongs` and `perps` receive each one's distance down the line and off
    // it. Sorted by perpendicular distance, closest first.
    int NearLine(float ox, float oz, float ux, float uz, float maxRange,
                 Place* out, float* alongs, float* perps, int n);

    // The same question asked one distance band at a time.
    //
    // Sorting the whole line by distance from it answers "what is nearly under
    // the crosshair" and hides "there is nothing out there at all", because
    // eight entries two metres off the line at sixteen hundred metres crowd
    // out the only thing at three hundred. Session eighty-six is exactly that:
    // two presses whose eight closest were all either thirteen metres away or
    // sixteen hundred, with nothing between, and no way to tell whether the
    // middle was empty or merely outranked.
    //
    // Band by band there is nowhere to hide. One line per band, the placement
    // closest to the line inside it, or a line saying the band holds nothing.
    // `edges` is `bandCount + 1` ranges in metres.
    void LogBands(float ox, float oy, float oz, float ux, float uz, float slope,
                  const float* edges, int bandCount);

    // The placements nearest a point, for the log.
    int Near(float px, float pz, Place* out, int n);

    // Every string each record's table holds, with its index.
    //
    // A record shares one string table and an element points at it. Session
    // seventy-three read the first entry for every element and so gave a whole
    // record one name, which is why 17,728 placements came back as 256 names
    // with counts of one to eleven. The table itself is the catalogue: an
    // array of 0x20-byte descriptors, a character pointer then a length then a
    // hash, walked until one stops reading as a string.
    void LogCatalog(int maxRecords, int maxPerRecord);

    // Is this name something worth a pin?
    //
    // A name beginning "sector_" is one of the chunks the world is cut into
    // rather than an object, and pinning one puts a marker in the middle of a
    // field. Those are always refused. Past that, the ini's Kinds list decides:
    // empty accepts everything, otherwise the name has to contain one of its
    // fragments.
    bool Worth(const char* name);
}
