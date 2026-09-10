#include "game/scan.h"

#include <Windows.h>

#include "game/rtti.h"

namespace
{
    constexpr size_t kMaxNeedles = 8;

    // One raw hit, flat so the guarded frame below holds no C++ objects.
    struct RawHit
    {
        void* at;
        int needle;
    };

    // The compare over one region, kept in its own function with no C++ objects
    // in the frame so it can sit inside __try. Another thread is free to unmap a
    // region while we walk it, and VirtualQuery only told us what was true a
    // moment ago, so the guard is doing real work rather than being decorative.
    size_t ScanRegion(const uint8_t* base, size_t size,
                      const uintptr_t* needles, size_t needleCount,
                      RawHit* outBuf, size_t outCap)
    {
        size_t found = 0;
        __try
        {
            // Objects are pointer-aligned, so an 8-byte stride is not just a
            // speed-up, it is the only alignment a vptr can land on.
            const uintptr_t* p = reinterpret_cast<const uintptr_t*>(base);
            const size_t count = size / sizeof(uintptr_t);
            for (size_t i = 0; i < count && found < outCap; ++i)
            {
                const uintptr_t v = p[i];
                // Two or three needles, so a linear check beats anything clever
                // and keeps the inner loop branch-predictable on the common miss.
                for (size_t n = 0; n < needleCount; ++n)
                {
                    if (v != needles[n]) continue;
                    outBuf[found].at = const_cast<void*>(static_cast<const void*>(&p[i]));
                    outBuf[found].needle = static_cast<int>(n);
                    ++found;
                    break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Region went away underneath us. Whatever was found before the
            // fault is still valid; the rest of this region is simply skipped.
        }
        return found;
    }
}

namespace gs::scan
{
    bool StillValid(const void* object, uintptr_t vtableAddress)
    {
        if (!object || !vtableAddress) return false;
        if (!gs::rtti::Readable(object, sizeof(void*))) return false;
        return *static_cast<const uintptr_t*>(object) == vtableAddress;
    }

    Report FindPointers(const uintptr_t* needles, size_t needleCount,
                        std::vector<Hit>& out, size_t maxHits, uint64_t byteBudget)
    {
        Report rep{};
        if (!needles || needleCount == 0 || needleCount > kMaxNeedles) return rep;

        LARGE_INTEGER freq{}, start{}, stop{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const auto* addr = static_cast<const uint8_t*>(si.lpMinimumApplicationAddress);
        const auto* limit = static_cast<const uint8_t*>(si.lpMaximumApplicationAddress);

        // One scratch buffer reused per region rather than a vector push inside
        // the guarded frame, which cannot hold objects with destructors.
        RawHit buf[256];

        while (addr < limit && out.size() < maxHits)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            rep.regionsSeen++;

            const auto* regionBase = static_cast<const uint8_t*>(mbi.BaseAddress);
            const size_t regionSize = mbi.RegionSize;
            if (regionSize == 0) break;  // no forward progress, stop rather than spin

            const DWORD prot = mbi.Protect & 0xFF;
            const bool readable =
                prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
                prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
            const bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;

            // MEM_IMAGE is skipped on purpose. The vtable lives there and so does
            // every static pointer to it, and those are not objects. What we want
            // is a heap allocation, which the factory takes from the game's
            // allocator into private memory.
            const bool wanted = mbi.State == MEM_COMMIT && readable && !guarded &&
                                (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED);

            if (wanted)
            {
                if (rep.bytesScanned + regionSize > byteBudget)
                {
                    rep.budgetHit = true;
                    break;
                }
                const size_t room = maxHits - out.size();
                const size_t cap = room < 256 ? room : 256;
                const size_t got = ScanRegion(regionBase, regionSize, needles, needleCount, buf, cap);
                for (size_t i = 0; i < got; ++i)
                {
                    Hit h{};
                    h.object = buf[i].at;
                    h.needle = buf[i].needle;
                    h.regionBase = reinterpret_cast<uintptr_t>(regionBase);
                    h.regionSize = regionSize;
                    h.regionType = mbi.Type;
                    out.push_back(h);
                }
                rep.regionsScanned++;
                rep.bytesScanned += regionSize;
            }

            addr = regionBase + regionSize;
        }

        QueryPerformanceCounter(&stop);
        if (freq.QuadPart)
            rep.microseconds = (stop.QuadPart - start.QuadPart) * 1000000ull / freq.QuadPart;
        return rep;
    }
}
