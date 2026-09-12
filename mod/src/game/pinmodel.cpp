#include "game/pinmodel.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>

#include "core/log.h"
#include "game/player.h"
#include "game/rtti.h"
#include "game/signatures.h"

namespace
{
    // A record is {int64 id, float x, y, z, byte, byte} in 24 bytes. The map
    // runs to about thirteen thousand units from the origin in x and z and the
    // ground sits between roughly zero and fifteen hundred, so a record that
    // is really a record reads as a position and one that is not reads as
    // nonsense. This is the same check the level gimmick table's quaternion
    // gave for free: the data validates the offset.
    bool LooksLikePosition(float x, float y, float z)
    {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
        if (std::fabs(x) > 40000.0f || std::fabs(z) > 40000.0f) return false;
        if (y < -2000.0f || y > 8000.0f) return false;
        return std::fabs(x) + std::fabs(z) > 1.0f;
    }
}

namespace gs::pinmodel
{
    uintptr_t Submodule()
    {
        const uintptr_t actor = gs::player::Actor();
        if (!actor) return 0;
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(actor + gs::sig::kOff_Actor_Components), 8))
                return 0;
            const uintptr_t comps =
                *reinterpret_cast<const uintptr_t*>(actor + gs::sig::kOff_Actor_Components);
            if (comps < 0x10000) return 0;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(comps + gs::sig::kOff_Comp_PinSubmodule), 8))
                return 0;
            const uintptr_t sub =
                *reinterpret_cast<const uintptr_t*>(comps + gs::sig::kOff_Comp_PinSubmodule);
            if (sub < 0x10000 || (sub & 7) != 0) return 0;
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(sub), 0x400)) return 0;
            return sub;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // Any object that holds a vector of marker-shaped records, printed.
    // Returns how many candidate lists it found.
    int SweepObject(uintptr_t obj, const char* tag, uintptr_t want)
    {
        int found = 0;
        __try
        {
            for (uintptr_t off = 0x40; off + 16 <= 0x400; off += 8)
            {
                const uintptr_t data = *reinterpret_cast<const uintptr_t*>(obj + off);
                const uint32_t count = *reinterpret_cast<const uint32_t*>(obj + off + 8);
                const uint32_t cap   = *reinterpret_cast<const uint32_t*>(obj + off + 12);
                if (data < 0x10000 || (data & 7) != 0) continue;
                if (count == 0 || count > 4096 || cap < count || cap > 65536) continue;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(data),
                                        static_cast<size_t>(count) * gs::sig::kPinRecord))
                    continue;

                int good = 0;
                for (uint32_t i = 0; i < count && i < 64; ++i)
                {
                    const auto* r = reinterpret_cast<const uint8_t*>(data) +
                                    static_cast<size_t>(i) * gs::sig::kPinRecord;
                    float x, y, z;
                    memcpy(&x, r + 8, 4); memcpy(&y, r + 12, 4); memcpy(&z, r + 16, 4);
                    if (LooksLikePosition(x, y, z)) ++good;
                }
                // A majority, not a single one. Session eighty let two lists
                // of pointers through because one record in sixty-seven
                // happened to hold two small floats, and the real list never
                // got looked for.
                if (good * 2 < static_cast<int>(count < 64 ? count : 64)) continue;
                ++found;

                GS_LOG("[pins]   %s+%03llX: %u record(s) of %u at 0x%p, %d read as positions%s",
                       tag, static_cast<unsigned long long>(off), count, cap,
                       reinterpret_cast<void*>(data), good,
                       off == want ? "   <- where the notes say kind 0x15 lives" : "");
                for (uint32_t i = 0; i < count && i < 12; ++i)
                {
                    const auto* r = reinterpret_cast<const uint8_t*>(data) +
                                    static_cast<size_t>(i) * gs::sig::kPinRecord;
                    int64_t id; float x, y, z;
                    memcpy(&id, r, 8);
                    memcpy(&x, r + 8, 4); memcpy(&y, r + 12, 4); memcpy(&z, r + 16, 4);
                    GS_LOG("[pins]     [%u] id %lld at (%.1f, %.1f, %.1f) flags %02X %02X",
                           i, static_cast<long long>(id), x, y, z, r[20], r[21]);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return found;
    }

    // Failing the documented chain, every component the player carries.
    //
    // The note that traced +0x168 rated "this resolved object is the local
    // player" at medium confidence, and the whole path hangs off that. So if
    // the slot gives nothing, walk the component block itself. Seth has
    // markers scattered over the map from earlier sessions, including one at
    // (-9714.1, -4141.2), and a list holding those coordinates is the list
    // whatever offset it turns up at.
    void SweepComponents(const char* why)
    {
        const uintptr_t actor = gs::player::Actor();
        if (!actor) { GS_LOG("[pins] %s: no player actor", why); return; }
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(actor + gs::sig::kOff_Actor_Components), 8))
                return;
            const uintptr_t comps =
                *reinterpret_cast<const uintptr_t*>(actor + gs::sig::kOff_Actor_Components);
            if (comps < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(comps), 0x400))
                return;
            GS_LOG("[pins] %s: the documented slot gave nothing; walking the component block at 0x%p",
                   why, reinterpret_cast<void*>(comps));
            int hits = 0;
            for (uintptr_t c = 0; c + 8 <= 0x400; c += 8)
            {
                const uintptr_t obj = *reinterpret_cast<const uintptr_t*>(comps + c);
                if (obj < 0x10000 || (obj & 7) != 0) continue;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(obj), 0x400)) continue;
                char tag[24];
                _snprintf_s(tag, sizeof(tag), _TRUNCATE, "comp+%03llX ",
                            static_cast<unsigned long long>(c));
                hits += SweepObject(obj, tag, 0xFFFFFFFF);
            }
            GS_LOG("[pins] %s: %d list(s) of marker-shaped records across the whole block", why, hits);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GS_LOG_ERR("[pins] a read faulted while walking the component block");
        }
    }

    // Coordinates from markers Seth placed by hand in earlier sessions, read
    // straight out of the spy captures of the game building them. Any of these
    // appearing as a float is the marker list, wherever it turns out to live.
    const float kKnownX[] = {
        -9714.073f, -10521.472f, -12386.253f, -11320.633f, -4464.704f,
        -12402.748f, -12675.004f, -4935.399f,
    };

    bool IsKnownX(float v)
    {
        for (float k : kKnownX)
            if (v > k - 0.05f && v < k + 0.05f) return true;
        return false;
    }

    // Every four-byte float in a block, checked against those. On a hit, the
    // bytes either side, because the record layout is the other half of what
    // this is for.
    int HuntBlock(uintptr_t base, size_t bytes, const char* tag)
    {
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(base), bytes)) return 0;
        int hits = 0;
        __try
        {
            for (size_t off = 0; off + 4 <= bytes; off += 4)
            {
                float v;
                memcpy(&v, reinterpret_cast<const void*>(base + off), 4);
                if (!IsKnownX(v)) continue;
                ++hits;
                GS_LOG("[pins] HIT %s+%04llX holds %.3f", tag,
                       static_cast<unsigned long long>(off), v);
                const size_t from = off >= 32 ? off - 32 : 0;
                for (size_t r = from; r < off + 48 && r + 16 <= bytes; r += 16)
                {
                    const auto* b = reinterpret_cast<const uint8_t*>(base + r);
                    float f0, f1, f2, f3;
                    memcpy(&f0, b, 4); memcpy(&f1, b + 4, 4);
                    memcpy(&f2, b + 8, 4); memcpy(&f3, b + 12, 4);
                    GS_LOG("[pins]   %s+%04llX  %02X%02X%02X%02X %02X%02X%02X%02X "
                           "%02X%02X%02X%02X %02X%02X%02X%02X   %.2f %.2f %.2f %.2f",
                           tag, static_cast<unsigned long long>(r),
                           b[3], b[2], b[1], b[0], b[7], b[6], b[5], b[4],
                           b[11], b[10], b[9], b[8], b[15], b[14], b[13], b[12],
                           f0, f1, f2, f3);
                }
                if (hits >= 4) break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return hits;
    }

    void HuntByCoordinates(const char* why)
    {
        const uintptr_t sub = Submodule();
        const uintptr_t actor = gs::player::Actor();
        if (!actor) { GS_LOG("[pins] %s: no player actor to hunt from", why); return; }

        int hits = 0;
        char tag[32];

        // The submodule itself, then everything it points at.
        if (sub)
        {
            hits += HuntBlock(sub, 0x1000, "sub");
            for (uintptr_t off = 0; off + 8 <= 0x1000 && hits < 8; off += 8)
            {
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(sub + off), 8)) break;
                const uintptr_t p = *reinterpret_cast<const uintptr_t*>(sub + off);
                if (p < 0x10000 || (p & 7) != 0) continue;
                _snprintf_s(tag, sizeof(tag), _TRUNCATE, "sub+%03llX*",
                            static_cast<unsigned long long>(off));
                hits += HuntBlock(p, 0x4000, tag);
            }
        }

        // Then every component the player carries, and what each points at.
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(actor + gs::sig::kOff_Actor_Components), 8))
                return;
            const uintptr_t comps =
                *reinterpret_cast<const uintptr_t*>(actor + gs::sig::kOff_Actor_Components);
            if (comps < 0x10000) return;
            for (uintptr_t c = 0; c + 8 <= 0x400 && hits < 8; c += 8)
            {
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(comps + c), 8)) break;
                const uintptr_t obj = *reinterpret_cast<const uintptr_t*>(comps + c);
                if (obj < 0x10000 || (obj & 7) != 0 || obj == sub) continue;
                _snprintf_s(tag, sizeof(tag), _TRUNCATE, "c%03llX",
                            static_cast<unsigned long long>(c));
                hits += HuntBlock(obj, 0x1000, tag);
                for (uintptr_t off = 0; off + 8 <= 0x400 && hits < 8; off += 8)
                {
                    if (!gs::rtti::Readable(reinterpret_cast<const void*>(obj + off), 8)) break;
                    const uintptr_t p = *reinterpret_cast<const uintptr_t*>(obj + off);
                    if (p < 0x10000 || (p & 7) != 0) continue;
                    _snprintf_s(tag, sizeof(tag), _TRUNCATE, "c%03llX+%03llX*",
                                static_cast<unsigned long long>(c),
                                static_cast<unsigned long long>(off));
                    hits += HuntBlock(p, 0x2000, tag);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        GS_LOG("[pins] %s: the coordinate hunt found %d hit(s)", why, hits);
    }

    void LogState(const char* why)
    {
        const uintptr_t sub = Submodule();
        if (!sub)
        {
            GS_LOG("[pins] %s: no submodule at actor+0x%llX -> +0x%llX yet", why,
                   static_cast<unsigned long long>(gs::sig::kOff_Actor_Components),
                   static_cast<unsigned long long>(gs::sig::kOff_Comp_PinSubmodule));
            SweepComponents(why);
            return;
        }
        const char* cls = gs::rtti::VtableClassName(
            *reinterpret_cast<const void* const*>(sub));
        GS_LOG("[pins] %s: submodule 0x%p%s%s", why, reinterpret_cast<void*>(sub),
               cls ? ", class " : "", cls ? cls : "");

        // The list the notes name, and its neighbours. If 0xC8 plus kind
        // times sixteen is right then one of these reads as a vector of
        // positions Seth will recognise; if it is wrong, the sweep says where
        // the right one is instead.
        const uintptr_t want = gs::sig::kOff_Pin_Lists +
                               static_cast<uintptr_t>(gs::sig::kPinKind) * 16;
        const int hits = SweepObject(sub, "", want);
        if (hits == 0)
        {
            GS_LOG("[pins] the submodule holds no list of marker-shaped records");
            SweepComponents(why);
        }
        HuntByCoordinates(why);
    }
}
