#pragma once

#include <Windows.h>
#include <cstdint>

// What the mod costs the game, measured rather than guessed. Once a minute
// the log gets one line per place the mod runs on the game's own threads
// (calls, total and worst time), the VirtualQuery calls the mod made, and
// the CPU each of the mod's own threads used.
namespace gs::load
{
    enum Site
    {
        // On the game's own threads.
        kTickPulse,     // the quarter second pulse in the minimap tick, all of it
        kPulseSave,     //   its save events
        kPulseRestore,  //   putting pins back on a new world
        kPulseFlush,    //   pins queued until the map was opened
        kPulseRetires,  //   pins the game dropped
        kPulseAuto,     //   the automatic marker
        kPulseClear,    //   taking pins off that are done
        kAutoRay,       //     the automatic marker's view ray
        kAutoSet,       //     its bearing over the entity set
        kAutoTable,     //     its bearing over the glint table
        kAutoAim,       //     asking the game's detect system
        kPlacePin,      // one pin placed, wherever it was asked for
        kTickMark,      // a key or chord press answered in the minimap tick
        kWorldMap,      // the world map's update while the map is open
        kIconCreate,    // the icon create spy, both surfaces
        kIconRemove,    // the icon remove spy, both surfaces
        kPickUp,        // the pick up message hook, the mod's part only
        kGameSites,
        // On the mod's own threads.
        kRefresh = kGameSites,   // the entity set, all of it
        kRefreshPools,           //   walking the manager's pools
        kRecover,                // looking for the player again
        kPadPump,                // the pad and the chord
        kSaveRead,               // every save record read
        kSaveLive,               // the loaded gimmicks' live state and pickups
        kSavePublish,            // the taken set and the state map
        kTableLoad,              // the glint table's count and read
        kSiteCount
    };

    // Times a scope on the game's thread and books it to a site.
    class Timer
    {
    public:
        explicit Timer(Site s);
        ~Timer();
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;

    private:
        Site site_;
        LARGE_INTEGER start_;
    };

    void Book(Site s, int64_t ticks);
    // What one site took the last time it ran, and forgetting it, so a slow
    // pass can say which of its parts was slow.
    double LastMs(Site s);
    void ClearLast(Site s);
    void Frame();                // one minimap tick, for the frame rate beside the costs
    void CountQuery();
    void NameThisThread(const char* name);   // for threads the mod did not create
    void AddThread(const char* name, HANDLE h);
    void Report(uint32_t nowMs);
}
