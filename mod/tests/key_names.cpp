// Compiles the real settings.cpp and reads Key values through it: every name
// in the ini's own list, the hex codes old inis hold, and the names GitHub #29
// found read as a piece of a hex code. Each key the inimeta offers has to come
// back as itself, since INI Master writes those names.
#include <Windows.h>
#include <cstdio>

namespace gs::Log
{
    void Write(const char*, const char*, ...) {}
}

#include "core/settings.cpp"

namespace
{
    int failures = 0;

    void Check(const char* text, uint32_t want)
    {
        const uint32_t got = KeyFromText(text);
        const bool ok = got == want;
        printf("%s  Key=%-14s reads %02X%s\n", ok ? "ok    " : "FAILED", text, got,
               ok ? "" : " and should not");
        if (!ok) ++failures;
    }
}

int main()
{
    printf("-- names --\n");
    Check("F9", 0x78);
    Check("f24", 0x87);
    Check("A", 0x41);
    Check("7", 0x37);
    Check("Numpad3", 0x63);
    Check("ScrollLock", 0x91);
    Check("PageUp", 0x21);
    Check("LBRACKET", 0xDB);
    Check("LeftBracket", 0xDB);
    Check("RightBracket", 0xDD);
    Check("Backslash", 0xDC);
    Check("0", 0x30);

    printf("-- the rest of the inimeta's list --\n");
    Check("Insert", 0x2D);   Check("Delete", 0x2E);   Check("Home", 0x24);
    Check("End", 0x23);      Check("PageDown", 0x22); Check("Pause", 0x13);
    Check("CapsLock", 0x14); Check("NumLock", 0x90);  Check("Space", 0x20);
    Check("Tilde", 0xC0);    Check("Minus", 0xBD);    Check("Equals", 0xBB);
    Check("Semicolon", 0xBA); Check("Quote", 0xDE);   Check("Comma", 0xBC);
    Check("Period", 0xBE);   Check("Slash", 0xBF);    Check("Numpad0", 0x60);
    Check("Numpad9", 0x69);  Check("F1", 0x70);       Check("Z", 0x5A);

    printf("-- hex codes from old inis --\n");
    Check("91", 0x91);
    Check("0x78", 0x78);
    Check("DE", 0xDE);

    printf("-- what is not a key this reads --\n");
    Check("Down", 0);
    Check("Escape", 0);
    Check("Enter", 0);
    Check("Apps", 0);
    Check("Ctrl+F11", 0);
    Check("100", 0);
    Check("00", 0);
    Check("", 0);

    puts("");
    puts(failures ? "SOMETHING FAILED" : "all cases pass");
    return failures ? 1 : 0;
}
