#include <Windows.h>
#include "core/mod.h"

static HMODULE g_module = nullptr;

static DWORD WINAPI MainThread(LPVOID)
{
    gs::Mod::Initialize(g_module);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_module = module;
        DisableThreadLibraryCalls(module);
#if GS_STAGE >= 1
        // Real work happens off the loader lock.
        if (HANDLE h = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr)) CloseHandle(h);
#endif
        break;
    case DLL_PROCESS_DETACH:
#if GS_STAGE >= 1
        // reserved is non-null when the process is terminating.
        gs::Mod::Shutdown(reserved != nullptr);
#endif
        break;
    }
    return TRUE;
}
