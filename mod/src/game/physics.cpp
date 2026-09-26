#include "game/physics.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/rtti.h"
#include "game/signatures.h"
#include "game/typescan.h"

namespace
{
    // The wrapper's first 27 bytes: mov rax,rsp; four register homes; push rbp,
    // r14, r15; sub rsp,0x200. Ten functions in the image start this way, so
    // the two stack argument reads that follow within 0x60 bytes are required
    // too: mov rdi,[rsp+0x240] and vmovss xmm8,[rsp+0x248].
    const uint8_t kPrologue[27] = {
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18,
        0x4C, 0x89, 0x60, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC};
    const uint8_t kArgDir[8]  = {0x48, 0x8B, 0xBC, 0x24, 0x40, 0x02, 0x00, 0x00};
    const uint8_t kArgDist[9] = {0xC5, 0x7A, 0x10, 0x84, 0x24, 0x48, 0x02, 0x00, 0x00};

    using CastFn = bool (*)(void* unused, int layer, bool flag, const float* start, const float* dir,
                            float maxDist, float* outDist, float* outNormal, bool* outFlag);

    uintptr_t g_base = 0;
    size_t g_size = 0;
    uintptr_t g_wrapper = 0;
    uintptr_t g_facade = 0;
    uintptr_t g_frameOff = 0;
    // 0 not looked for, 1 being looked for, 2 done. The look reads the whole
    // code section and took 70 ms on the game's thread the first time a pin
    // wanted a line of sight on 26 September, so the table thread does it
    // once the world is up, and a cast that arrives while it runs is told
    // not ready rather than made to wait.
    std::atomic<int> g_located{0};
    bool g_saidWhy = false;

    bool Base()
    {
        if (g_base) return true;
        return gs::typescan::ModuleRange(g_base, g_size);
    }

    bool InImage(uintptr_t p) { return p >= g_base && p < g_base + g_size; }

    bool ReadQ(uintptr_t at, uintptr_t* out)
    {
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 8)) return false;
            *out = *reinterpret_cast<const uintptr_t*>(at);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool Has(const uint8_t* hay, size_t n, const uint8_t* needle, size_t m)
    {
        if (m > n) return false;
        for (size_t i = 0; i + m <= n; ++i)
            if (memcmp(hay + i, needle, m) == 0) return true;
        return false;
    }

    // rip-relative target of an instruction whose disp32 sits at `at`, with
    // the instruction ending `tail` bytes after the disp32.
    uintptr_t RipTarget(uintptr_t at, int tail = 0)
    {
        int32_t disp;
        memcpy(&disp, reinterpret_cast<const void*>(at), 4);
        return at + 4 + tail + static_cast<intptr_t>(disp);
    }

    // Find the wrapper in the module's code and decode its operands. Runs once.
    void Locate()
    {
        if (g_located.load() != 0 || !Base()) return;
        int expected = 0;
        if (!g_located.compare_exchange_strong(expected, 1)) return;
        uintptr_t at = g_base;
        __try
        {
            while (at < g_base + g_size && !g_wrapper)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi))) break;
                const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress), re = rb + mbi.RegionSize;
                const bool code = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                                  (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
                if (code)
                {
                    const uint8_t* p = reinterpret_cast<const uint8_t*>(rb);
                    const size_t n = re - rb;
                    for (size_t i = 0; i + 0x220 <= n; ++i)
                    {
                        if (p[i] != 0x48 || memcmp(p + i, kPrologue, sizeof(kPrologue)) != 0) continue;
                        if (!Has(p + i + sizeof(kPrologue), 0x60, kArgDir, sizeof(kArgDir))) continue;
                        if (!Has(p + i + sizeof(kPrologue), 0x60, kArgDist, sizeof(kArgDist))) continue;
                        const uintptr_t fn = rb + i;
                        // The facade: mov rax,[rip+d] (48 8B 05) followed by lea rcx,[rip+d']
                        // (48 8D 0D) with both resolving to the same address.
                        uintptr_t facade = 0, frame = 0;
                        for (size_t k = 0; k + 14 <= 0x220; ++k)
                        {
                            const uint8_t* q = p + i + k;
                            if (!facade && q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x05 && q[7] == 0x48 && q[8] == 0x8D && q[9] == 0x0D)
                            {
                                const uintptr_t a = RipTarget(fn + k + 3), b = RipTarget(fn + k + 10);
                                if (a == b) facade = a;
                            }
                            // vsubps xmm4, xmm1, [rip+d]: C5 F0 5C 25
                            if (!frame && q[0] == 0xC5 && q[1] == 0xF0 && q[2] == 0x5C && q[3] == 0x25)
                                frame = RipTarget(fn + k + 4);
                        }
                        if (facade && frame)
                        {
                            g_wrapper = fn;
                            g_facade = facade;
                            g_frameOff = frame;
                            break;
                        }
                    }
                }
                at = re;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        if (g_wrapper)
        {
            GS_LOG_OK("[physics] ray cast wrapper found by pattern at RVA 0x%llX; facade RVA 0x%llX, frame offset RVA 0x%llX%s",
                      static_cast<unsigned long long>(g_wrapper - g_base), static_cast<unsigned long long>(g_facade - g_base),
                      static_cast<unsigned long long>(g_frameOff - g_base),
                      g_wrapper - g_base == gs::sig::kRayCastWrapper ? " (as recorded)" : " (moved since the record)");
        }
        else
        {
            g_wrapper = g_base + gs::sig::kRayCastWrapper;
            g_facade = g_base + gs::sig::kPhysicsFacade;
            g_frameOff = g_base + gs::sig::kPhysicsFrameOff;
            GS_LOG_ERR("[physics] the ray cast wrapper's pattern was not found; using the recorded RVAs, which the prologue check may refuse");
        }
        g_located.store(2);
    }

    bool CallGuarded(CastFn fn, void* facade, int layer, bool flag, const float* start, const float* dir,
                     float maxDist, float* outDist, float* outNormal, bool* outFlag, bool* result)
    {
        __try
        {
            *result = fn(facade, layer, flag, start, dir, maxDist, outDist, outNormal, outFlag);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

namespace gs::physics
{
    void Warm()
    {
        if (Base()) Locate();
    }

    bool Ready(const char** why)
    {
        if (!Base()) { *why = "module range unknown"; return false; }
        Locate();
        if (g_located.load() != 2) { *why = "the ray cast wrapper is still being looked for"; return false; }
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(g_wrapper), sizeof(kPrologue)) ||
            memcmp(reinterpret_cast<const void*>(g_wrapper), kPrologue, sizeof(kPrologue)) != 0)
        {
            *why = "the wrapper does not start with the analysed bytes";
            return false;
        }
        uintptr_t vt = 0, slot = 0;
        if (!ReadQ(g_facade, &vt) || !InImage(vt)) { *why = "the facade's vtable is not in the image"; return false; }
        if (!ReadQ(vt + sig::kSlotCastRay * 8, &slot) || !InImage(slot)) { *why = "the facade's castRay slot is not in the image"; return false; }
        return true;
    }

    Hit Cast(const float* start, const float* dir, float maxDist, int layer, bool flag)
    {
        Hit h;
        const char* why = nullptr;
        if (!Ready(&why))
        {
            if (!g_saidWhy) { g_saidWhy = true; GS_LOG_ERR("[physics] not ready: %s", why); }
            return h;
        }
        float s[4] = {start[0], start[1], start[2], 0};
        float d[4] = {dir[0], dir[1], dir[2], 0};
        float dist = 0;
        float normal[4] = {0, 0, 0, 0};
        bool f = false;
        bool result = false;
        const CastFn fn = reinterpret_cast<CastFn>(g_wrapper);
        if (!CallGuarded(fn, reinterpret_cast<void*>(g_facade), layer, flag, s, d, maxDist, &dist, normal, &f, &result))
        {
            GS_LOG_ERR("[physics] the cast faulted (layer %d); no hit", layer);
            return h;
        }
        if (!result || !std::isfinite(dist) || dist < 0.0f) return h;
        h.hit = true;
        h.dist = dist;
        h.normal[0] = normal[0]; h.normal[1] = normal[1]; h.normal[2] = normal[2];
        h.flag = f;
        return h;
    }

    Sight LineOfSight(const float* eye, const float* target, int steps, float* blockedAt,
                      float* blockedHeight)
    {
        const char* why = nullptr;
        if (!Ready(&why)) return Sight::Unknown;
        if (steps < 2) steps = 2;
        if (steps > 32) steps = 32;

        const float dx = target[0] - eye[0];
        const float dy = target[1] - eye[1];
        const float dz = target[2] - eye[2];
        const float flat = std::sqrt(dx * dx + dz * dz);
        if (flat < 1.0f) return Sight::Unknown;

        // Clearance, so that standing on a slope or a target sitting on the
        // ground does not read as a hill. Two metres of the line, plus a
        // little more at range where a sample is a coarser thing.
        const float down[3] = {0.0f, -1.0f, 0.0f};
        int measured = 0;
        int blockedRun = 0;
        for (int i = 1; i < steps; ++i)
        {
            // Skip the last tenth: the target is usually standing on the
            // ground and its own hill is not an obstruction.
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            if (t > 0.9f) break;
            const float px = eye[0] + dx * t;
            const float py = eye[1] + dy * t;
            const float pz = eye[2] + dz * t;
            const float from[3] = {px, py + 300.0f, pz};
            const Hit h = Cast(from, down, 1200.0f, 0, false);
            if (!h.hit) continue;               // collision not loaded here
            ++measured;
            const float groundY = from[1] - h.dist;
            // Eight metres, plus three percent of how far along the sample is.
            //
            // It was two metres plus one percent, which is under three metres
            // at eighty, and session a hundred and eighteen had that refuse a
            // glint I was looking straight at. The thing this is for is a
            // mountain between me and something a kilometre away, and a
            // mountain is not three metres. One sample over the line is a bank
            // at the side of a road, so it takes two in a row.
            const float clearance = 8.0f + flat * t * 0.03f;
            // Above the line is the whole test. A second clause requiring the
            // ground to clear the target as well was tried on 22 September
            // and taken back: looking down, the line never drops below the
            // target, so the clause changed nothing; looking up, it cleared
            // ridges that do cut the line. No caller refuses on this now.
            if (groundY > py + clearance)
            {
                if (++blockedRun >= 2)
                {
                    if (blockedAt) *blockedAt = flat * t;
                    if (blockedHeight) *blockedHeight = groundY;
                    return Sight::Blocked;
                }
            }
            else
            {
                blockedRun = 0;
            }
        }
        return measured > 0 ? Sight::Clear : Sight::Unknown;
    }

    void LogState()
    {
        const char* why = nullptr;
        const bool ok = Ready(&why);
        uintptr_t vt = 0;
        ReadQ(g_facade, &vt);
        float off[4] = {0, 0, 0, 0};
        if (g_frameOff && gs::rtti::Readable(reinterpret_cast<const void*>(g_frameOff), 16))
            memcpy(off, reinterpret_cast<const void*>(g_frameOff), 16);
        GS_LOG("[physics] %s; wrapper 0x%p, facade vtable 0x%p, frame offset (%.1f, %.1f, %.1f, %.1f)",
               ok ? "ready" : why, reinterpret_cast<void*>(g_wrapper), reinterpret_cast<void*>(vt), off[0], off[1], off[2], off[3]);
    }
}
