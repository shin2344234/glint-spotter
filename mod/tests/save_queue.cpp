// Compiles the real saveslot.cpp and fills its queue the way the title screen
// does. Every open waits there for the first world tick, and the last open is
// the one that names the save, so a full queue must not lose it. Through 1.1.39
// it did. Stubs stand in for the log and the import table.
#include <Windows.h>
#include <cstdarg>
#include <cstdio>

namespace gs::Log
{
    void Write(const char*, const char*, ...) {}
}

namespace gs::iathook
{
    bool Swap(const char*, const char*, void*, void** previous)
    {
        if (previous) *previous = nullptr;
        return false;
    }
}

#include "game/saveslot.cpp"

namespace
{
    int failures = 0;
    constexpr uint32_t kAccount = 12800898;

    void Check(const char* what, bool ok)
    {
        printf("%s  %s", ok ? "ok    " : "FAILED", what);
        puts("");
        if (!ok) ++failures;
    }

    gs::saveslot::Id Slot(int n)
    {
        gs::saveslot::Id id;
        id.account = kAccount;
        id.slot = n;
        return id;
    }

    void Open(int n) { gs::saveslot::Id id = Slot(n); std::lock_guard<std::mutex> l(g_mutex); PushLocked(id, false, false); }
    void Wrote(int n) { gs::saveslot::Id id = Slot(n); std::lock_guard<std::mutex> l(g_mutex); PushLocked(id, true, false); }
    void Read(int n) { gs::saveslot::Id id = Slot(n); std::lock_guard<std::mutex> l(g_mutex); PushLocked(id, false, true); }

    // What PumpSaveEvents does: sixteen at a time, up to eight rounds.
    struct Drained
    {
        gs::saveslot::Event ev[256];
        int n = 0;
        int rounds = 0;
    };

    Drained Pump()
    {
        Drained d;
        gs::saveslot::Event ev[16];
        for (int round = 0; round < 8; ++round)
        {
            const int got = gs::saveslot::Take(ev, 16);
            ++d.rounds;
            for (int i = 0; i < got; ++i) d.ev[d.n++] = ev[i];
            if (got < 16) break;
        }
        return d;
    }

    // The slot the restore would go by: the last plain open.
    int LastPeek(const Drained& d)
    {
        int slot = -1;
        for (int i = 0; i < d.n; ++i)
            if (!d.ev[i].write && !d.ev[i].full) slot = d.ev[i].id.slot;
        return slot;
    }
}

int main()
{
    // One launch on this machine, from the 6 November log: 14 slots, then 12,
    // then the real load.
    for (int i = 0; i < 14; ++i) Open(i);
    for (int i = 0; i < 12; ++i) Open(i);
    Open(103);
    Check("a queue that does not fill drops nothing", g_dropped == 0);
    Drained one = Pump();
    Check("all 27 come out", one.n == 27);
    Check("in the order they went in", one.ev[0].id.slot == 0 && one.ev[14].id.slot == 0);
    Check("and the last open is the save being loaded", LastPeek(one) == 103);

    // Five trips through a load menu of 14 saves, then the load: 71 opens.
    for (int pass = 0; pass < 5; ++pass)
        for (int i = 0; i < 14; ++i) Open(i);
    Open(103);
    Check("71 opens overflow the queue", g_dropped == 7);
    Drained two = Pump();
    Check("the queue hands back what it holds", two.n == 64);
    Check("the save being loaded survives an overflow", LastPeek(two) == 103);

    // A save and a full read near the front, then a flood of opens.
    Wrote(5);
    Read(7);
    for (int i = 0; i < 200; ++i) Open(i % 14);
    Open(102);
    Drained three = Pump();
    Check("a save outlives the flood", three.ev[0].write && three.ev[0].id.slot == 5);
    Check("a full read outlives the flood", three.ev[1].full && three.ev[1].id.slot == 7);
    Check("and the newest open is still last", LastPeek(three) == 102);

    // Nothing left behind for the next tick.
    Drained four = Pump();
    Check("the queue ends empty", four.n == 0 && g_queued == 0);

    puts("");
    puts(failures ? "SOMETHING FAILED" : "all cases pass");
    return failures ? 1 : 0;
}
