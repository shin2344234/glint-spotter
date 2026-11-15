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

    // Buffered since GitHub #24: a line is on the disk within a few tenths of
    // a second without a Shutdown, no line from four threads at once is lost
    // or torn, and a line far past the stack buffer comes out whole.
    Clean();
    gs::Log::Start(nullptr);
    gs::Log::Write("info ", "before the wait");
    Sleep(500);
    {
        FILE* f = _wfsopen((g_base + L".log").c_str(), L"r", _SH_DENYNO);
        std::string seen;
        if (f)
        {
            char b[512];
            while (fgets(b, sizeof(b), f)) seen += b;
            fclose(f);
        }
        Check("a line reaches the disk without a shutdown", seen.find("before the wait") != std::string::npos);
    }
    const std::string longText(5000, 'x');
    gs::Log::Write("info ", "long %s end", longText.c_str());
    HANDLE th[4];
    for (int t = 0; t < 4; ++t)
        th[t] = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            const int id = static_cast<int>(reinterpret_cast<intptr_t>(p));
            for (int i = 0; i < 5000; ++i) gs::Log::Write("info ", "thread %d line %d", id, i);
            return 0;
        }, reinterpret_cast<LPVOID>(static_cast<intptr_t>(t)), 0, nullptr);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    for (HANDLE h : th) CloseHandle(h);
    gs::Log::Write("error", "last line");
    gs::Log::Shutdown();
    {
        FILE* f = nullptr;
        int threadLines = 0, longOk = 0, torn = 0;
        if (_wfopen_s(&f, (g_base + L".log").c_str(), L"r") == 0 && f)
        {
            std::string all;
            char b[8192];
            while (fgets(b, sizeof(b), f)) all += b;
            fclose(f);
            size_t at = 0;
            while (at < all.size())
            {
                size_t end = all.find('\n', at);
                if (end == std::string::npos) end = all.size();
                const std::string line = all.substr(at, end - at);
                if (line.find("thread ") != std::string::npos)
                {
                    int a = -1, b2 = -1;
                    const size_t p = line.find("thread ");
                    if (sscanf_s(line.c_str() + p, "thread %d line %d", &a, &b2) == 2 && line[0] == '[') ++threadLines;
                    else ++torn;
                }
                if (line.find("long " + longText + " end") != std::string::npos) ++longOk;
                at = end + 1;
            }
            Check("every line from four threads is in the file", threadLines == 20000);
            Check("no line is torn", torn == 0);
            Check("a 5000 character line comes out whole", longOk == 1);
            Check("the line written just before shutdown is kept", all.find("error last line") != std::string::npos);
        }
        else Check("the log can be read after shutdown", false);
    }

    Clean();
    if (failures) printf("\n%d failed\n", failures);
    return failures ? 1 : 0;
}
