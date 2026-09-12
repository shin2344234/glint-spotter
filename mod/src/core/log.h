#pragma once

namespace gs::Log
{
    // printf-style, one line per call. Opens GlintSpotter.log next to the plugin
    // on the first write and keeps it open. Start() rotates the previous five
    // sessions to .01 through .05 so a run can be read against the one before it,
    // the way Master Looter's logs are.
    void Start(void* selfModule);
    void Write(const char* level, const char* fmt, ...);
    void Shutdown();
}

#define GS_LOG(...)     ::gs::Log::Write("info ", __VA_ARGS__)
#define GS_LOG_OK(...)  ::gs::Log::Write("ok   ", __VA_ARGS__)
#define GS_LOG_ERR(...) ::gs::Log::Write("error", __VA_ARGS__)
