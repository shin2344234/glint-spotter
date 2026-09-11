#include "game/dump.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/rtti.h"

namespace gs::dump
{
    void Object(const char* tag, uintptr_t obj, size_t bytes)
    {
        __try
        {
            size_t n = bytes;
            while (n >= 0x40 && !gs::rtti::Readable(reinterpret_cast<const void*>(obj), n)) n /= 2;
            if (n < 0x40) return;
            const auto* f = reinterpret_cast<const float*>(obj);
            const auto* u = reinterpret_cast<const uint32_t*>(obj);
            char line[400];
            for (size_t off = 0; off + 32 <= n; off += 32)
            {
                int w = 0;
                for (int k = 0; k < 8; ++k)
                {
                    const size_t i = off / 4 + k;
                    const float v = f[i];
                    int r;
                    if (std::isfinite(v) && (v == 0.0f || (std::fabs(v) > 1e-4f && std::fabs(v) < 1e6f)))
                        r = _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " %11.4f", v);
                    else
                        r = _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, "  0x%08X ", u[i]);
                    if (r < 0) break;
                    w += r;
                }
                line[w] = 0;
                GS_LOG("[%s +%03zX]%s", tag, off, line);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void Pointers(const char* tag, uintptr_t obj, size_t bytes)
    {
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(obj), bytes)) return;
            const auto* q = reinterpret_cast<const uintptr_t*>(obj);
            const auto* u = reinterpret_cast<const uint32_t*>(obj);
            for (size_t i = 0; (i + 1) * 8 <= bytes; ++i)
            {
                const uintptr_t v = q[i];
                const size_t off = i * 8;
                // An entity id is a dword whose top byte is 0xA0 for the player
                // and 0xB0 for the world, so both halves of the qword are worth
                // a look before it is judged as a pointer.
                for (int half = 0; half < 2; ++half)
                {
                    const uint32_t e = u[i * 2 + half];
                    const uint32_t top = e >> 24;
                    if (top == 0xA0 || top == 0xB0)
                        GS_LOG("[%s +%03zX] entity id %08X", tag, off + half * 4, e);
                }
                if (v < 0x10000 || (v & 7) != 0) continue;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(v), 8)) continue;
                const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(v);
                const char* n = (vt >= 0x10000 && gs::rtti::Readable(reinterpret_cast<const void*>(vt), 8))
                                    ? gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt))
                                    : nullptr;
                if (n) GS_LOG("[%s +%03zX] 0x%p is %s", tag, off, reinterpret_cast<void*>(v), n);
                else GS_LOG("[%s +%03zX] 0x%p, no class", tag, off, reinterpret_cast<void*>(v));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    int EffectActivity(uintptr_t entity, size_t bytes)
    {
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(entity + 0x68), 8)) return -1;
            const uintptr_t comps = *reinterpret_cast<const uintptr_t*>(entity + 0x68);
            if (comps < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(comps), 0x80)) return -1;
            const uintptr_t eff = *reinterpret_cast<const uintptr_t*>(comps + 0x60);
            if (eff < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(eff), bytes)) return -1;
            int set = 0;
            const auto* q = reinterpret_cast<const uintptr_t*>(eff);
            for (size_t i = 1; i * 8 < bytes; ++i)
            {
                const uintptr_t v = q[i];
                if (v >= 0x10000 && v <= 0x00007FFFFFFFFFFFull && (v & 7) == 0) ++set;
            }
            return set;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    void GimmickState(const char* tag, uintptr_t entity, size_t bytes)
    {
        uintptr_t comp = 0, sub = 0;
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(entity + 0x68), 8)) return;
            const uintptr_t comps = *reinterpret_cast<const uintptr_t*>(entity + 0x68);
            if (comps < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(comps), 0x80)) return;
            comp = *reinterpret_cast<const uintptr_t*>(comps + 0x30);
            if (comp < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(comp), 0x440)) return;
            sub = *reinterpret_cast<const uintptr_t*>(comp + 0x438);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        Object(tag, comp, bytes);
        if (sub >= 0x10000)
        {
            char t[64];
            _snprintf_s(t, sizeof(t), _TRUNCATE, "%ssub", tag);
            Object(t, sub, 0x200);
        }
    }

    void EntityComponents(const char* tag, uintptr_t entity, size_t bytesEach)
    {
        struct Slot { uintptr_t off; const char* name; };
        const Slot slots[] = {{0x20, "status"}, {0x30, "gimmick"}, {0x50, "detect"}, {0x60, "effect"}};
        uintptr_t comps = 0;
        __try
        {
            if (!gs::rtti::Readable(reinterpret_cast<const void*>(entity + 0x68), 8)) return;
            comps = *reinterpret_cast<const uintptr_t*>(entity + 0x68);
            if (comps < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(comps), 0x80)) return;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        for (const Slot& s : slots)
        {
            uintptr_t c = 0;
            __try
            {
                c = *reinterpret_cast<const uintptr_t*>(comps + s.off);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                c = 0;
            }
            if (c < 0x10000 || (c & 7) != 0 || !gs::rtti::Readable(reinterpret_cast<const void*>(c), 8)) continue;
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(c);
            const char* n = gs::rtti::VtableClassName(reinterpret_cast<const void*>(vt));
            char t[64];
            _snprintf_s(t, sizeof(t), _TRUNCATE, "%s %s", tag, s.name);
            GS_LOG("[%s] component +0x%02llX at 0x%p is %s", t, static_cast<unsigned long long>(s.off),
                   reinterpret_cast<void*>(c), n ? (n[0] == '.' ? n + 4 : n) : "?");
            Object(t, c, bytesEach);
        }
    }
}
