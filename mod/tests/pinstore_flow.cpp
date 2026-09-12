// Drives the real pinstore through the sequences a player actually produces,
// including the two that lost pins in the first sessions. The pin file lands
// beside this exe, so each case starts by deleting it.
#include <Windows.h>
#include <cstdio>
#include <cstring>

#include "core/settings.h"

namespace gs::Log
{
    void Write(const char* level, const char* fmt, ...) { (void)level; (void)fmt; }
}

namespace gs::Settings
{
    static Values g_values;
    const Values& Get() { return g_values; }
}

#include "core/pinstore.cpp"

namespace
{
    int failures = 0;
    const uint32_t kAcct = 12800898;

    void Fresh()
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring p(buf);
        p.resize(p.find_last_of(L'.'));
        DeleteFileW((p + L".pins").c_str());
        gs::pinstore::Load(nullptr);
    }

    void Reload() { gs::pinstore::Load(nullptr); }

    void Expect(const char* what, int want)
    {
        gs::pinstore::Saved got[64];
        const int n = gs::pinstore::All(got, 64);
        if (n == want)
        {
            printf("ok    %-56s %d pin(s)\n", what, n);
            return;
        }
        ++failures;
        printf("FAIL  %-56s wanted %d, got %d\n", what, want, n);
    }

    void Mark(float x) { gs::pinstore::Add(x, 0.0f, 0.0f, "pin"); }
}

int main()
{
    gs::Settings::g_values.keepPins = true;

    printf("-- a save the mod has never seen, marks, then a second save --\n");
    Fresh();
    gs::pinstore::Loaded(kAcct, 3);
    Mark(1); Mark(2);
    Expect("slot3 after two marks", 2);
    gs::pinstore::Loaded(kAcct, 1);
    Expect("slot1, which is somebody else's save", 0);
    gs::pinstore::Loaded(kAcct, 3);
    Expect("slot3 again", 2);

    printf("\n-- saving into a slot never used before carries them --\n");
    gs::pinstore::Loaded(kAcct, 3);
    gs::pinstore::SavedTo(kAcct, 104);
    Expect("straight after saving into slot104", 2);
    gs::pinstore::Loaded(kAcct, 1);
    Expect("away to slot1", 0);
    gs::pinstore::Loaded(kAcct, 104);
    Expect("back through the new slot104", 2);

    printf("\n-- all of it survives a restart --\n");
    Reload();
    gs::pinstore::Loaded(kAcct, 104);
    Expect("slot104 after a reload", 2);
    gs::pinstore::Loaded(kAcct, 3);
    Expect("slot3 after a reload", 2);
    gs::pinstore::Loaded(kAcct, 1);
    Expect("slot1 after a reload", 0);

    printf("\n-- an autosave slot changing hands --\n");
    gs::pinstore::Loaded(kAcct, 3);
    gs::pinstore::SavedTo(kAcct, 0);
    Expect("slot0 now belongs to slot3's playthrough", 2);
    gs::pinstore::Loaded(kAcct, 1);
    Mark(9);
    gs::pinstore::SavedTo(kAcct, 0);
    Expect("slot1 took slot0 over", 1);
    gs::pinstore::Loaded(kAcct, 0);
    Expect("loading slot0 gives the one that took it", 1);
    gs::pinstore::Loaded(kAcct, 3);
    Expect("slot3 still has its own", 2);

    printf("\n-- a new game --\n");
    gs::pinstore::NewGame();
    Expect("a new game starts empty", 0);
    Mark(5); Mark(6); Mark(7);
    gs::pinstore::SavedTo(kAcct, 200);
    Expect("its three marks follow its first save", 3);
    Reload();
    gs::pinstore::Loaded(kAcct, 200);
    Expect("and are there after a restart", 3);

    printf("\n-- pins written by 1.0, with no save against them --\n");
    Fresh();
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring p(buf);
        p.resize(p.find_last_of(L'.'));
        FILE* f = nullptr;
        _wfopen_s(&f, (p + L".pins").c_str(), L"w");
        fputs("; old file\n-1.0 2.0 -3.0 Glint\n-4.0 5.0 -6.0 821m\n", f);
        fclose(f);
    }
    Reload();
    Expect("read back before any save is known", 2);
    gs::pinstore::Loaded(kAcct, 3);
    Expect("adopted by the first save loaded", 2);
    gs::pinstore::Loaded(kAcct, 1);
    Expect("and not handed to the next one", 0);

    printf("\n-- a session where the watch never fires --\n");
    Fresh();
    Mark(1); Mark(2); Mark(3);
    Expect("three marks with no save known", 3);
    Reload();
    Expect("still there next launch, which 1.1.0 got wrong", 3);
    gs::pinstore::Drop(2.0f, 0.0f);
    Expect("and the map can still take one off", 2);

    printf("\n%s\n", failures ? "SOMETHING FAILED" : "all cases pass");
    return failures ? 1 : 0;
}
