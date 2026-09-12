#include "game/alert.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/rtti.h"
#include "game/signatures.h"
#include "hook/vtable.h"

// Shared with thunk.asm.
extern "C" void* gs_alertOriginal = nullptr;
extern "C" void gs_AlertThunk();

namespace
{
    gs::vtable::Swap g_swap;
    std::atomic<uint64_t> g_seen{0};

    // The map spy's rule, and it was the right one: the first handful of calls
    // say everything a replay needs, and the rest would bury it.
    constexpr uint64_t kFullDumps = 16;

    // Any pointer that reads as printable ASCII, copied out. Returns false
    // when it does not, which is most of them.
    bool AsText(const void* p, char* out, size_t cap)
    {
        if (!p || !gs::rtti::Readable(p, 2)) return false;
        const char* s = static_cast<const char*>(p);
        size_t i = 0;
        for (; i + 1 < cap; ++i)
        {
            if (!gs::rtti::Readable(s + i, 1)) break;
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == 0) break;
            if (c < 0x20 || c > 0x7E) return false;
            out[i] = static_cast<char>(c);
        }
        out[i] = 0;
        return i >= 2;
    }

    void DumpBytes(const char* tag, const void* p, size_t bytes)
    {
        if (!p || !gs::rtti::Readable(p, bytes)) { GS_LOG("[alert]   %s unreadable", tag); return; }
        const auto* b = static_cast<const uint8_t*>(p);
        char line[200];
        int w = 0;
        for (size_t i = 0; i < bytes && w + 4 < static_cast<int>(sizeof(line)); ++i)
            w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, "%02X ", b[i]);
        line[w] = 0;
        GS_LOG("[alert]   %s %s", tag, line);
    }

    // The fourth argument, which the disassembly says is {tag byte, data at
    // +8, count at +0x10} over entries of 0x18 bytes.
    void DumpArgList(const void* p)
    {
        if (!p || !gs::rtti::Readable(p, 0x18)) { GS_LOG("[alert]   arg list unreadable"); return; }
        const auto* b = static_cast<const uint8_t*>(p);
        const uint8_t tag = b[0];
        const void* data = *reinterpret_cast<void* const*>(b + 8);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(b + 0x10);
        GS_LOG("[alert]   arg list tag 0x%02X, %u entr%s at 0x%p", tag, count,
               count == 1 ? "y" : "ies", data);
        if (!data || count > 64 || !gs::rtti::Readable(data, static_cast<size_t>(count) * 0x18)) return;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto* e = static_cast<const uint8_t*>(data) + static_cast<size_t>(i) * 0x18;
            char label[32];
            _snprintf_s(label, sizeof(label), _TRUNCATE, "[%u] kind 0x%02X", i, e[0]);
            DumpBytes(label, e, 0x18);
            // The handler reads a length at +0x10 for the entry it wants, so
            // +8 is the obvious place for the characters. Try both a pointer
            // there and the bytes themselves.
            char text[96];
            const void* at8 = *reinterpret_cast<void* const*>(e + 8);
            if (AsText(at8, text, sizeof(text)))
                GS_LOG("[alert]     +8 points at \"%s\"", text);
            else if (AsText(e + 8, text, sizeof(text)))
                GS_LOG("[alert]     +8 reads as \"%s\"", text);
        }
    }
}

// Called from the thunk with the four register arguments and a pointer to the
// caller's fifth. Every register the game set is saved around this, so nothing
// here can disturb the call it is watching.
extern "C" void gs_OnAlert(void* self, void* a2, void* a3, void* a4, void** stackArgs)
{
    const uint64_t n = ++g_seen;
    if (n > kFullDumps)
    {
        if ((n % 200) == 0) GS_LOG("[alert] %llu calls so far", static_cast<unsigned long long>(n));
        return;
    }
    __try
    {
        GS_LOG("[alert] call #%llu on 0x%p, thread %lu", static_cast<unsigned long long>(n),
               self, GetCurrentThreadId());
        GS_LOG("[alert]   a2 0x%p  a3 0x%p  a4 0x%p", a2, a3, a4);
        char text[96];
        if (AsText(a2, text, sizeof(text))) GS_LOG("[alert]   a2 reads \"%s\"", text);
        if (AsText(a3, text, sizeof(text))) GS_LOG("[alert]   a3 reads \"%s\"", text);
        DumpArgList(a4);
        if (stackArgs && gs::rtti::Readable(stackArgs, 32))
            GS_LOG("[alert]   stack args 0x%p 0x%p 0x%p 0x%p",
                   stackArgs[0], stackArgs[1], stackArgs[2], stackArgs[3]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        GS_LOG_ERR("[alert] a read faulted while describing call #%llu; the call itself is untouched",
                   static_cast<unsigned long long>(n));
    }
}

namespace gs::alert
{
    bool InstallSpy(uintptr_t alertVtable)
    {
        if (!alertVtable || g_swap.installed) return false;
        if (!gs::vtable::Install(alertVtable, gs::sig::kSlotAlertCall,
                                 reinterpret_cast<void*>(&gs_AlertThunk), g_swap))
        {
            GS_LOG_ERR("[alert] could not take slot %d on the alert root vtable", gs::sig::kSlotAlertCall);
            return false;
        }
        gs_alertOriginal = g_swap.original;
        GS_LOG_OK("[alert] slot %d on 0x%p was 0x%p, now the thunk; every alert the game raises is "
                  "written down", gs::sig::kSlotAlertCall, reinterpret_cast<void*>(alertVtable),
                  g_swap.original);
        return true;
    }

    void RemoveSpy()
    {
        if (!g_swap.installed) return;
        bool leftAlone = false;
        if (gs::vtable::Restore(g_swap, leftAlone)) GS_LOG("[alert] slot restored");
        else if (leftAlone) GS_LOG("[alert] slot now holds someone else's hook, left in place");
    }

    uint64_t Seen() { return g_seen.load(); }
}
