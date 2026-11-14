// Compiles the real log.cpp and starts it against an ini beside this exe, the
// way the plugin starts it beside itself, to check that Logs= keeps that many
// files in all and deletes the rest, including the .03 to .05 an older build
// left behind. Diggyp586 asked for a single log on 10 November, GitHub #7.
#include <Windows.h>
#include <cstdio>
#include <string>

#include "core/log.cpp"

namespace
{
    int failures = 0;
    std::wstring g_base;

    void Check(const char* what, bool ok)
    {
        printf("%s  %s", ok ? "ok    " : "FAILED", what);
        puts("");
        if (!ok) ++failures;
    }

    std::wstring Numbered(int i)
    {
        wchar_t suffix[16]{};
        swprintf_s(suffix, L".%02d.log", i);
        return g_base + suffix;
    }

    bool Exists(const std::wstring& p)
    {
        return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    void Put(const std::wstring& p, const char* text)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, p.c_str(), L"w") == 0 && f) { fputs(text, f); fclose(f); }
    }

    std::string Text(const std::wstring& p)
    {
        FILE* f = nullptr;
        std::string out;
        if (_wfopen_s(&f, p.c_str(), L"r") != 0 || !f) return out;
        char buf[512];
        while (fgets(buf, sizeof(buf), f)) out += buf;
        fclose(f);
        return out;
    }

    void Clean()
    {
        DeleteFileW((g_base + L".log").c_str());
        DeleteFileW((g_base + L".ini").c_str());
        for (int i = 1; i <= 99; ++i) DeleteFileW(Numbered(i).c_str());
    }

    // The log and five older ones, the way 1.1.40 leaves bin64.
    void SeedFive()
    {
        Put(g_base + L".log", "run 0\n");
        for (int i = 1; i <= 5; ++i)
        {
            char t[16];
            sprintf_s(t, "run %d\n", i);
            Put(Numbered(i), t);
        }
    }

    void Launch(const char* ini)
    {
        if (ini) Put(g_base + L".ini", ini);
        gs::Log::Start(nullptr);
        gs::Log::Write("info ", "this run");
        gs::Log::Shutdown();
    }

    int OldCount()
    {
        int n = 0;
        for (int i = 1; i <= 99; ++i) if (Exists(Numbered(i))) ++n;
        return n;
    }
}

int main()
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    g_base.assign(buf, n);
    g_base.resize(g_base.find_last_of(L'.'));

    Clean();
    SeedFive();
    Launch(nullptr);
    Check("no ini keeps two: this run and .01", Exists(g_base + L".log") && OldCount() == 1);
    Check("no ini: .01 holds the run before", Text(Numbered(1)) == "run 0\n");
    Check("no ini: .02 to .05 from an older build are deleted", !Exists(Numbered(2)) && !Exists(Numbered(5)));
    Check("no ini: the log says it is the default",
          Text(g_base + L".log").find("log: Logs=2 (the default), this run's log and the one before it") !=
              std::string::npos);

    Clean();
    SeedFive();
    Launch("[GlintSpotter]\n; a comment naming Logs=7\nLogs=1\n");
    Check("Logs=1 keeps no older file", OldCount() == 0);
    Check("Logs=1 overwrites the log", Text(g_base + L".log").find("run 0") == std::string::npos &&
                                           Text(g_base + L".log").find("this run") != std::string::npos);
    Check("Logs=1 says so", Text(g_base + L".log").find("log: Logs=1, only this run's log is kept") !=
                                std::string::npos);
    Launch(nullptr);
    Check("Logs=1 a second launch still keeps one", OldCount() == 0 && Exists(g_base + L".log"));

    Clean();
    SeedFive();
    Launch("[GlintSpotter]\n  logs = 4\n");
    Check("Logs=4 keeps .01 to .03", OldCount() == 3 && Exists(Numbered(3)) && !Exists(Numbered(4)));
    Check("Logs=4 shuffles each one down", Text(Numbered(1)) == "run 0\n" && Text(Numbered(3)) == "run 2\n");

    Clean();
    SeedFive();
    Launch("[GlintSpotter]\nLogs=6\n");
    Check("Logs=6 keeps the five older ones", OldCount() == 5 && Text(Numbered(5)) == "run 4\n");

    Clean();
    SeedFive();
    Launch("[GlintSpotter]\nLogs=0\n");
    Check("Logs=0 is refused and two are kept", OldCount() == 1);
    Check("Logs=0 is named in the log",
          Text(g_base + L".log").find("log: Logs in the ini is out of range (1 to 99), keeping 2 logs") !=
              std::string::npos);

    Clean();
    if (failures) printf("\n%d failed\n", failures);
    return failures ? 1 : 0;
}
