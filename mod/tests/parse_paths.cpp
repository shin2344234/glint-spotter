// Compiles the real saveslot.cpp and calls its parser on the paths the game
// was actually seen opening, plus the ones it must refuse. Stubs stand in for
// the log and the import table so nothing else has to be linked.
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

    void Case(const wchar_t* path, bool want, uint32_t account = 0, int slot = -1)
    {
        gs::saveslot::Id id;
        const bool got = Parse(path, &id);
        bool ok = got == want;
        if (ok && want) ok = id.account == account && id.slot == slot;
        if (!ok)
        {
            ++failures;
            wprintf(L"FAIL  %ls\n", path);
            printf("      wanted %s %u/%d, got %s %u/%d\n", want ? "yes" : "no", account, slot,
                   got ? "yes" : "no", id.account, id.slot);
        }
        else
        {
            wprintf(L"ok    %ls\n", path);
        }
    }
}

int main()
{
    // Exactly as the log printed them, lowercase and forward slashes.
    Case(L"c:/users/seth/appdata/local/pearl abyss/cd/save/12800898/slot0/save.save", true,
         12800898, 0);
    Case(L"c:/users/seth/appdata/local/pearl abyss/cd/save/12800898/slot105/save.save", true,
         12800898, 105);
    Case(L"C:\\Users\\seth\\AppData\\Local\\Pearl Abyss\\CD\\save\\762179015\\slot3\\save.save",
         true, 762179015, 3);
    Case(L"c:/users/seth/appdata/local/pearl abyss/cd/save/12800898/SLOT7/SAVE.SAVE", true,
         12800898, 7);

    // The menu's own file, which must never count as a load.
    Case(L"c:/users/seth/appdata/local/pearl abyss/cd/save/12800898/slot0/lobby.save", false);
    // Not under a save folder.
    Case(L"d:/games/crimson desert/bin64/slot3/save.save", false);
    // A slot with no number, and a folder that only looks like one.
    Case(L"c:/x/save/12800898/slot/save.save", false);
    Case(L"c:/x/save/12800898/slotwhat/save.save", false);
    // An account that is not digits.
    Case(L"c:/x/save/steamuser/slot3/save.save", false);
    // A file whose name merely ends the same way.
    Case(L"c:/x/save/12800898/slot3/autosave.save", false);
    // Truncated things that must not walk off the front.
    Case(L"save.save", false);
    Case(L"/save.save", false);
    Case(L"", false);

    printf("\n%s\n", failures ? "SOMETHING FAILED" : "all cases pass");
    return failures ? 1 : 0;
}
