#include "game/scan.h"

#include <Windows.h>

#include "game/rtti.h"

namespace
{
    constexpr size_t kMaxNeedles = 64;

    // One raw hit, flat so the guarded frame below holds no C++ objects.
    struct RawHit
    {
        size_t offset;
        int needle;
    };

    // The compare over one region, kept in its own function with no C++ objects
    // in the frame so it can sit inside __try. Another thread is free to unmap a
    // region while we walk it, and VirtualQuery only told us what was true a
    // moment ago, so the guard is doing real work rather than being decorative.
    //
    // objectBytes is applied here rather than by the caller: the whole region is
    // known committed and readable, so a hit with that much room behind it needs
    // no second probe, and one with less is noise that never leaves this loop.
    size_t ScanRegion(const uint8_t* base, size_t size,
                      const uintptr_t* needles, size_t needleCount,
                      const size_t* needleBytes, RawHit* outBuf, size_t outCap,
                      size_t* rawMatches, size_t* rejectedNoRoom)
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
                    ++*rawMatches;
                    const size_t off = i * sizeof(uintptr_t);
                    if (off + needleBytes[n] > size)
                    {
                        ++*rejectedNoRoom;
                        break;
                    }
                    outBuf[found].offset = off;
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
                        std::vector<Hit>& out, const Options& opt)
    {
        Report rep{};
        if (!needles || needleCount == 0 || needleCount > kMaxNeedles) return rep;
        if (!opt.needleBytes) return rep;

        // The cheapest region reject: one too small to hold even the smallest
        // thing we are looking for cannot hold any of them.
        size_t smallest = static_cast<size_t>(-1);
        for (size_t i = 0; i < needleCount; ++i)
            if (opt.needleBytes[i] < smallest) smallest = opt.needleBytes[i];

        LARGE_INTEGER freq{}, start{}, now{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        const long long budgetTicks =
            freq.QuadPart ? static_cast<long long>(opt.timeBudgetMs) * freq.QuadPart / 1000 : 0;

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const auto* addr = static_cast<const uint8_t*>(si.lpMinimumApplicationAddress);
        const auto* limit = static_cast<const uint8_t*>(si.lpMaximumApplicationAddress);

        // One scratch buffer reused per region rather than a vector push inside
        // the guarded frame, which cannot hold objects with destructors.
        RawHit buf[256];

        while (addr < limit && out.size() < opt.maxHits)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            rep.regionsSeen++;

            const auto* regionBase = static_cast<const uint8_t*>(mbi.BaseAddress);
            const size_t regionSize = mbi.RegionSize;
            if (regionSize == 0) break;  // no forward progress, stop rather than spin

            // A UI control is a heap allocation: private, committed, read-write.
            // MEM_IMAGE holds the vtable itself and every static pointer to it,
            // which are not objects, and executable pages are code.
            const DWORD prot = mbi.Protect & 0xFF;
            const bool writable = prot == PAGE_READWRITE ||
                                  (opt.wideKinds && (prot == PAGE_WRITECOPY ||
                                                     prot == PAGE_EXECUTE_READWRITE ||
                                                     prot == PAGE_EXECUTE_WRITECOPY ||
                                                     prot == PAGE_READONLY));
            const bool typeOk = mbi.Type == MEM_PRIVATE ||
                                (opt.wideKinds && mbi.Type == MEM_MAPPED);
            const bool kindOk = mbi.State == MEM_COMMIT && typeOk && writable &&
                                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;

            if (!kindOk)
            {
                rep.regionsSkippedKind++;
            }
            else if (regionSize < smallest)
            {
                rep.regionsSkippedSmall++;
            }
            else if (regionSize > opt.maxRegionBytes)
            {
                rep.regionsSkippedLarge++;
                rep.bytesSkippedLarge += regionSize;
            }
            else
            {
                const size_t room = opt.maxHits - out.size();
                const size_t cap = room < 256 ? room : 256;
                const size_t got = ScanRegion(regionBase, regionSize, needles, needleCount,
                                              opt.needleBytes, buf, cap,
                                              &rep.rawMatches, &rep.rejectedNoRoom);
                for (size_t i = 0; i < got; ++i)
                {
                    Hit h{};
                    h.object = const_cast<uint8_t*>(regionBase) + buf[i].offset;
                    h.needle = buf[i].needle;
                    h.regionBase = reinterpret_cast<uintptr_t>(regionBase);
                    h.regionSize = regionSize;
                    out.push_back(h);
                }
                rep.regionsScanned++;
                rep.bytesScanned += regionSize;

                if (budgetTicks)
                {
                    QueryPerformanceCounter(&now);
                    if (now.QuadPart - start.QuadPart > budgetTicks)
                    {
                        rep.timeBudgetHit = true;
                        break;
                    }
                }
            }

            addr = regionBase + regionSize;
        }

        QueryPerformanceCounter(&now);
        if (freq.QuadPart)
            rep.microseconds = (now.QuadPart - start.QuadPart) * 1000000ull / freq.QuadPart;
        return rep;
    }
}
