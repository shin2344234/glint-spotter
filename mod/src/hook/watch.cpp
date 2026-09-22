#include "hook/watch.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/actors.h"
#include "game/aim.h"
#include "game/player.h"
#include "game/rtti.h"
#include "game/savemap.h"
#include "core/settings.h"

namespace
{
    // The two fields of the special mode component the flash's clear routine
    // resets together (re-detecttarget-2850.md, "clears this+0x40/this+0x50").
    // +0x40 holds the player id while the flash is up; it is the flag aim.cpp
    // reads.
    struct Field { const char* name; uintptr_t off; };
    const Field kFlashFields[2] = {
        {"flash flag +0x40", 0x40},
        {"special +0x50", 0x50},
    };
    // Watch=2: the two list counts on the player's server
    // ServerContentsMiscActorComponent (savemap.h), writes only. The list at
    // +0x48 is what the save writes as the map's completed placements, and it
    // read empty for a whole session, so whatever fills it does so briefly.
    const Field kSaveFields[2] = {
        {"placement list count +0x50", 0x50},
        {"second list count +0x80", 0x80},
    };
    const Field* kFields = kFlashFields;
    constexpr int kFieldCount = 2;
    int g_mode = 1;
    // In the debug registers: RW 11 LEN 11 is read or write four bytes, RW 01
    // LEN 11 is write four bytes.
    DWORD64 g_kind = 0xF;

    std::atomic<uintptr_t> g_target{0};     // the component the registers point into
    uintptr_t g_armedTarget = 0;
    uintptr_t g_base = 0, g_end = 0;        // the game's image
    uintptr_t g_selfLo = 0, g_selfHi = 0;   // this plugin, whose own reads are skipped
    bool g_ready = false;
    PVOID g_veh = nullptr;

    // One per instruction that touched a field. Filled from the exception
    // handler, so fixed size and lock free: a slot is claimed by swapping its
    // rip in, and its registers are copied once with the flash off and once
    // with it on.
    struct Regs { uint64_t v[16]; uint64_t stack[32]; uint32_t flag; uint32_t tid; };
    struct Site
    {
        std::atomic<uint64_t> key{0};   // rip, with the field in bit 60
        std::atomic<uint32_t> count{0}, countOn{0}, changed{0};
        std::atomic<int> offState{0}, onState{0};   // 0 empty, 1 filling, 2 ready
        Regs off, on;
        uint32_t loggedCount = 0, loggedOn = 0;
        bool logged = false, loggedOffRegs = false, loggedOnRegs = false;
    };
    constexpr int kSites = 256;
    Site g_sites[kSites];
    std::atomic<uint32_t> g_full{0}, g_self{0};
    std::atomic<uint32_t> g_last[kFieldCount];

    void Copy(const CONTEXT* c, Regs& r, uint32_t flag)
    {
        const uint64_t v[16] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp,
                                c->R8,  c->R9,  c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
        memcpy(r.v, v, sizeof(v));
        memcpy(r.stack, reinterpret_cast<const void*>(c->Rsp), sizeof(r.stack));
        r.flag = flag;
        r.tid = GetCurrentThreadId();
    }

    LONG CALLBACK OnException(EXCEPTION_POINTERS* ep)
    {
        if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT* c = ep->ContextRecord;
        const DWORD64 dr6 = c->Dr6;
        int field = -1;
        for (int i = 0; i < kFieldCount; ++i)
            if (dr6 & (1ull << i)) field = i;
        if (field < 0) return EXCEPTION_CONTINUE_SEARCH;   // not ours
        c->Dr6 = 0;

        // A data breakpoint traps after the instruction, so rip is the next
        // one. The plugin's own reads of the flag are not what is wanted.
        const uint64_t rip = c->Rip;
        if (rip >= g_selfLo && rip < g_selfHi)
        {
            g_self.fetch_add(1, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        const uintptr_t target = g_target.load(std::memory_order_relaxed);
        const uint32_t now = target ? *reinterpret_cast<const volatile uint32_t*>(target + kFields[field].off) : 0;
        const uint32_t was = g_last[field].exchange(now, std::memory_order_relaxed);
        const uint32_t flag = !target ? 0 : g_mode == 2 ? now
                                                          : *reinterpret_cast<const volatile uint32_t*>(target + 0x40);

        Site* s = nullptr;
        const uint64_t key = rip | (static_cast<uint64_t>(field) << 60);
        const uint32_t h = static_cast<uint32_t>((key * 0x9E3779B97F4A7C15ull) >> 56);
        for (int n = 0; n < kSites; ++n)
        {
            Site& t = g_sites[(h + n) % kSites];
            uint64_t cur = t.key.load(std::memory_order_acquire);
            if (cur == 0)
            {
                uint64_t expected = 0;
                if (t.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel))
                {
                    s = &t;
                    break;
                }
                cur = expected;
            }
            if (cur == key) { s = &t; break; }
        }
        if (!s)
        {
            g_full.fetch_add(1, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        s->count.fetch_add(1, std::memory_order_relaxed);
        if (now != was) s->changed.fetch_add(1, std::memory_order_relaxed);
        if (flag) s->countOn.fetch_add(1, std::memory_order_relaxed);

        std::atomic<int>& state = flag ? s->onState : s->offState;
        int empty = 0;
        if (state.compare_exchange_strong(empty, 1, std::memory_order_acquire))
        {
            Copy(c, flag ? s->on : s->off, flag);
            state.store(2, std::memory_order_release);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void FindSelf()
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&OnException), &self) || !self)
            return;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
        g_selfLo = reinterpret_cast<uintptr_t>(self);
        g_selfHi = g_selfLo + nt->OptionalHeader.SizeOfImage;
    }

    void FindGame()
    {
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
        g_end = g_base + nt->OptionalHeader.SizeOfImage;
    }

    uint32_t g_armed[2048];
    int g_armedN = 0;

    bool Armed(uint32_t tid)
    {
        for (int i = 0; i < g_armedN; ++i) if (g_armed[i] == tid) return true;
        return false;
    }

    // The debug registers of one thread, pointed at the watched fields or
    // cleared. RW 11 is read or write, LEN 11 is four bytes.
    bool Apply(HANDLE t, uintptr_t target)
    {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(t, &c)) return false;
        DWORD64 dr7 = c.Dr7;
        for (int i = 0; i < kFieldCount; ++i)
        {
            const DWORD64 local = 1ull << (i * 2);
            const DWORD64 kind = 0xFull << (16 + i * 4);
            if (target)
            {
                (&c.Dr0)[i] = target + kFields[i].off;
                dr7 = (dr7 & ~kind) | (g_kind << (16 + i * 4)) | local;
            }
            else
            {
                (&c.Dr0)[i] = 0;
                dr7 &= ~(local | kind);
            }
        }
        c.Dr7 = dr7;
        return SetThreadContext(t, &c) != 0;
    }

    // Each other thread is stopped only for the two context calls. Nothing
    // that allocates or takes a lock runs while one is stopped.
    //
    // The calling thread is never suspended: it would never come back to
    // resume itself. That happens when Disarm runs on a game thread that was
    // armed, which is the thread a FreeLibrary arrives on. Its own registers
    // are set through the current-thread handle instead.
    bool SetRegisters(uint32_t tid, uintptr_t target)
    {
        if (tid == GetCurrentThreadId()) return Apply(GetCurrentThread(), target);
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                              THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!t) return false;
        bool done = false;
        if (SuspendThread(t) != static_cast<DWORD>(-1))
        {
            done = Apply(t, target);
            ResumeThread(t);
        }
        CloseHandle(t);
        return done;
    }

    void Where(uint64_t at, char* out, size_t cap)
    {
        if (at >= g_base && at < g_end)
        {
            _snprintf_s(out, cap, _TRUNCATE, "+0x%llX", static_cast<unsigned long long>(at - g_base));
            return;
        }
        HMODULE m = nullptr;
        wchar_t path[MAX_PATH] = L"";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(at), &m) && m &&
            GetModuleFileNameW(m, path, MAX_PATH))
        {
            const wchar_t* name = wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            _snprintf_s(out, cap, _TRUNCATE, "%ls+0x%llX", name,
                        static_cast<unsigned long long>(at - reinterpret_cast<uintptr_t>(m)));
            return;
        }
        _snprintf_s(out, cap, _TRUNCATE, "0x%llX", static_cast<unsigned long long>(at));
    }

    void Describe(const char* what, uint64_t v, char* out, size_t cap)
    {
        gs::actors::Entity e;
        const char* role = nullptr;
        if (v >= 0x10000 && gs::actors::Owner(static_cast<uintptr_t>(v), &e, &role))
        {
            const gs::player::Pos pp = gs::player::Read();
            const float dx = e.x - pp.x, dz = e.z - pp.z;
            _snprintf_s(out, cap, _TRUNCATE, "%s = %s of \"%s\" eid %08X at (%.1f, %.1f, %.1f), %.0f m away",
                        what, role, e.name[0] ? e.name : "?", e.eid, e.x, e.y, e.z,
                        pp.valid ? std::sqrt(dx * dx + dz * dz) : -1.0f);
            return;
        }
        if (v >= g_base && v < g_end)
        {
            _snprintf_s(out, cap, _TRUNCATE, "%s = image +0x%llX", what, static_cast<unsigned long long>(v - g_base));
            return;
        }
        // A heap object with a vtable in the image gets its class named.
        if (v >= 0x10000 && (v & 7) == 0 && gs::rtti::Readable(reinterpret_cast<const void*>(v), 8))
        {
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(v);
            if (vt >= g_base && vt < g_end)
            {
                const char* cls = gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt));
                if (cls && cls[0])
                {
                    _snprintf_s(out, cap, _TRUNCATE, "%s = 0x%llX, a %s", what, static_cast<unsigned long long>(v), cls);
                    return;
                }
            }
        }
        _snprintf_s(out, cap, _TRUNCATE, "%s = 0x%llX", what, static_cast<unsigned long long>(v));
    }

    const char* kRegNames[16] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                 "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

    int g_linesLeft = 900;

    void LogRegs(uint64_t rip, int field, const Regs& r, const char* when)
    {
        char at[96];
        Where(rip, at, sizeof(at));
        GS_LOG("[watch] registers at %s (%s) with %s %s, thread %u, value 0x%X:", at,
               kFields[field].name, g_mode == 2 ? "the count" : "the flash", when, r.tid, r.flag);
        g_linesLeft -= 1;
        for (int i = 0; i < 16; ++i)
        {
            if (i == 7) continue;   // rsp
            char d[240];
            Describe(kRegNames[i], r.v[i], d, sizeof(d));
            GS_LOG("[watch]     %s", d);
            --g_linesLeft;
        }
        // Return addresses on the stack: every qword that lands in the image.
        char line[600];
        size_t used = 0;
        line[0] = 0;
        for (int i = 0; i < 32; ++i)
        {
            const uint64_t q = r.stack[i];
            if (q < g_base || q >= g_end) continue;
            used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, " [rsp+%X] +0x%llX", i * 8,
                                static_cast<unsigned long long>(q - g_base));
            if (used >= sizeof(line) - 40) break;
        }
        GS_LOG("[watch]     image addresses on the stack:%s", line[0] ? line : " none");
        --g_linesLeft;
    }
}

namespace gs::watch
{
    void Arm()
    {
        g_mode = gs::Settings::Get().watch == 2 ? 2 : 1;
        kFields = g_mode == 2 ? kSaveFields : kFlashFields;
        g_kind = g_mode == 2 ? 0xD : 0xF;
        const uintptr_t target = g_mode == 2 ? gs::savemap::Component() : gs::aim::SpecialComponent();
        if (!target) return;   // the object to watch is not in hand yet
        if (!g_ready)
        {
            FindGame();
            FindSelf();
            g_veh = AddVectoredExceptionHandler(1, OnException);
            g_ready = g_veh != nullptr;
            if (!g_ready) { GS_LOG_ERR("[watch] no exception handler; off"); return; }
            if (g_mode == 2)
                GS_LOG_OK("[watch] on: a write watch on the two list counts of the player's server "
                          "ServerContentsMiscActorComponent. Every instruction that writes them is logged once, "
                          "with its registers and the return addresses on its stack.");
            else
                GS_LOG_OK("[watch] on: a read or write watch on the flash flag at +0x40 and on +0x50 of the "
                          "special mode component. Every game instruction that touches them is logged once, "
                          "with its registers with the flash off and again with it up.");
        }
        g_target.store(target);
        if (target != g_armedTarget)
        {
            // A load hands over a new component; move every armed thread to it.
            for (int i = 0; i < kFieldCount; ++i)
                g_last[i].store(*reinterpret_cast<const volatile uint32_t*>(target + kFields[i].off));
            int moved = 0;
            for (int i = 0; i < g_armedN; ++i) moved += SetRegisters(g_armed[i], target) ? 1 : 0;
            GS_LOG("[watch] watching the special mode component at 0x%p%s", reinterpret_cast<void*>(target),
                   g_armedTarget ? ", moved from the last one" : "");
            if (g_armedTarget) GS_LOG("[watch] moved %d thread(s)", moved);
            g_armedTarget = target;
        }
        const DWORD pid = GetCurrentProcessId();
        const DWORD self = GetCurrentThreadId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        int added = 0;
        for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            if (Armed(te.th32ThreadID) || g_armedN >= 2048) continue;
            if (SetRegisters(te.th32ThreadID, target))
            {
                g_armed[g_armedN++] = te.th32ThreadID;
                ++added;
            }
        }
        CloseHandle(snap);
        if (added) GS_LOG("[watch] armed on %d more thread(s), %d in all", added, g_armedN);
    }

    void Drain()
    {
        if (!g_ready) return;
        const uint32_t now = GetTickCount();
        for (int i = 0; i < kSites && g_linesLeft > 0; ++i)
        {
            Site& s = g_sites[i];
            const uint64_t key = s.key.load(std::memory_order_acquire);
            if (!key) continue;
            const uint64_t rip = key & ((1ull << 60) - 1);
            const int field = static_cast<int>(key >> 60);
            if (!s.logged)
            {
                s.logged = true;
                char at[96];
                Where(rip, at, sizeof(at));
                GS_LOG("[watch] new: %s touched by the instruction before %s, flash %s", kFields[field].name, at,
                       gs::aim::FlashActive() ? "on" : "off");
                --g_linesLeft;
            }
            if (!s.loggedOffRegs && s.offState.load(std::memory_order_acquire) == 2)
            {
                s.loggedOffRegs = true;
                LogRegs(rip, field, s.off, g_mode == 2 ? "at zero" : "off");
            }
            if (!s.loggedOnRegs && s.onState.load(std::memory_order_acquire) == 2)
            {
                s.loggedOnRegs = true;
                LogRegs(rip, field, s.on, g_mode == 2 ? "above zero" : "up");
            }
        }

        static uint32_t lastSum = 0;
        if (now - lastSum > 30000)
        {
            lastSum = now;
            int n = 0, said = 0;
            for (int i = 0; i < kSites; ++i)
            {
                Site& s = g_sites[i];
                const uint64_t key = s.key.load(std::memory_order_acquire);
                if (!key) continue;
                const uint64_t rip = key & ((1ull << 60) - 1);
                const int field = static_cast<int>(key >> 60);
                ++n;
                const uint32_t c = s.count.load(), on = s.countOn.load();
                if (c == s.loggedCount || said >= 40) continue;
                ++said;
                char at[96];
                Where(rip, at, sizeof(at));
                GS_LOG("[watch]   %s at %s: %u touch(es), %u with the flash up, %u changed the value", at,
                       kFields[field].name, c, on, s.changed.load());
                s.loggedCount = c;
                s.loggedOn = on;
            }
            GS_LOG("[watch] totals: %d instruction(s) seen, %u of the plugin's own reads skipped, %u not "
                   "recorded for want of room", n, g_self.load(), g_full.load());
        }
    }

    void Disarm()
    {
        if (!g_ready) return;
        for (int i = 0; i < g_armedN; ++i) SetRegisters(g_armed[i], 0);
        g_armedN = 0;
        if (g_veh) RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
        g_ready = false;
    }
}
