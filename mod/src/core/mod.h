#pragma once

namespace gs::Mod
{
    // Called off the loader lock from a worker thread; see dllmain.cpp.
    void Initialize(void* selfModule);
    void Shutdown(bool processTerminating);
}
