#include "game/scan.h"

#include <Windows.h>
#include <psapi.h>

#include "game/rtti.h"

namespace
{
    constexpr size_t kMaxNeedles = 64;
    constexpr size_t kPageSize = 4096;
    constexpr size_t kPagesPerQuery = 1024;  // 4 MB of address space per call

    // One raw hit, flat so the guarded frame below holds no C++ objects.
    struct RawHit
    {
        size_t offset;   // from the start of the region, not the run
        int needle;
    };

    // The compare over one run of pages, kept in its own function with no C++
    // objects in the frame so it can sit inside __try. Another thread is free to
    // unmap memory while we walk it, and VirtualQuery only told us what was true
    // a moment ago, so the guard is doing real work rather than being decorative.
    //
    // The fit test measures against the whole region rather than this run,
    // because an object can start in a resident page and continue into one that
    // is not, and it is still an object.
    size_t ScanRun(const uint8_t* regionBase, size_t regionSize,
                   size_t runOffset, size_t runBytes,
                   const uintptr_t* needles, size_t needleCount,
                   uintptr_t lo, uintptr_t hi,
                   const size_t* needleBytes, RawHit* outBuf, size_t outCap,
                   size_t* rawMatches, size_t* rejectedNoRoom)
    {
        size_t found = 0;
        __try
        {
            // Objects are pointer-aligned, so an 8-byte stride is not just a
            // speed-up, it is the only alignment a vptr can land on.
            const uintptr_t* p = reinterpret_cast<const uintptr_t*>(regionBase + runOffset);
            const size_t count = runBytes / sizeof(uintptr_t);
            for (size_t i = 0; i < count && found < outCap; ++i)
            {
                const uintptr_t v = p[i];

                // Every needle is a vtable inside the game module, so one
                // unsigned compare against that span rejects almost every qword
                // before the loop below is reached.
                if (v - lo > hi - lo) continue;

                for (size_t n = 0; n < needleCount; ++n)
                {
                    if (v != needles[n]) continue;
                    ++*rawMatches;
                    const size_t off = runOffset + i * sizeof(uintptr_t);
                    if (off + needleBytes[n] > regionSize)
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
            // Memory went away underneath us. Whatever was found before the
            // fault is still valid; the rest of this run is simply skipped.
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
        __try
        {
            return *static_cast<const uintptr_t*>(object) == vtableAddress;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
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

        // The span every needle falls inside, for the pre-filter in ScanRun.
        uintptr_t lo = static_cast<uintptr_t>(-1), hi = 0;
        for (size_t i = 0; i < needleCount; ++i)
        {
            if (needles[i] < lo) lo = needles[i];
            if (needles[i] > hi) hi = needles[i];
        }

        LARGE_INTEGER freq{}, start{}, now{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        uint64_t lastYieldAt = 0;
        const long long budgetTicks =
            freq.QuadPart ? static_cast<long long>(opt.timeBudgetMs) * freq.QuadPart / 1000 : 0;

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const auto* addr = static_cast<const uint8_t*>(si.lpMinimumApplicationAddress);
        const auto* limit = static_cast<const uint8_t*>(si.lpMaximumApplicationAddress);
        const HANDLE self = GetCurrentProcess();

        RawHit buf[256];
        static PSAPI_WORKING_SET_EX_INFORMATION ws[kPagesPerQuery];

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

            const auto rb = reinterpret_cast<uintptr_t>(regionBase);
            const bool wantedOnly = opt.onlyRegionContaining == 0 ||
                (opt.onlyRegionContaining >= rb && opt.onlyRegionContaining < rb + regionSize);

            if (!kindOk || !wantedOnly) rep.regionsSkippedKind++;
            else if (regionSize < smallest || regionSize < opt.minRegionBytes) rep.regionsSkippedSmall++;
            else if (regionSize > opt.maxRegionBytes)
            {
                rep.regionsSkippedLarge++;
                rep.bytesSkippedLarge += regionSize;
            }
            else
            {
                // Reading a page that is committed but not resident costs a soft
                // fault, and one fault per page works out near 60 ns a qword.
                // That is why session four read 6.8 GB in a minute and still ran
                // out of time: about 90 MB/s, against the 4 GB/s the same loop
                // manages on memory already in the working set. So ask which
                // pages are resident and read only those.
                size_t offset = 0;
                while (offset < regionSize && out.size() < opt.maxHits)
                {
                    const size_t pagesLeft = (regionSize - offset + kPageSize - 1) / kPageSize;
                    const size_t pages = pagesLeft < kPagesPerQuery ? pagesLeft : kPagesPerQuery;

                    for (size_t i = 0; i < pages; ++i)
                        ws[i].VirtualAddress =
                            const_cast<uint8_t*>(regionBase + offset + i * kPageSize);

                    const bool queried = opt.residentOnly &&
                        QueryWorkingSetEx(self, ws,
                                          static_cast<DWORD>(pages * sizeof(ws[0]))) != FALSE;

                    size_t i = 0;
                    while (i < pages)
                    {
                        // With no residency information, treat everything as
                        // resident rather than skipping the region outright.
                        const bool valid = !queried || ws[i].VirtualAttributes.Valid != 0;
                        size_t run = 1;
                        while (i + run < pages &&
                               (!queried || ws[i + run].VirtualAttributes.Valid != 0) == valid)
                            ++run;

                        const size_t runOffset = offset + i * kPageSize;
                        size_t runBytes = run * kPageSize;
                        if (runOffset >= regionSize) break;
                        if (runOffset + runBytes > regionSize) runBytes = regionSize - runOffset;

                        if (!valid)
                        {
                            rep.bytesSkippedNotResident += runBytes;
                        }
                        else
                        {
                            const size_t room = opt.maxHits - out.size();
                            const size_t cap = room < 256 ? room : 256;
                            const size_t got = ScanRun(regionBase, regionSize, runOffset, runBytes,
                                                       needles, needleCount, lo, hi,
                                                       opt.needleBytes, buf, cap,
                                                       &rep.rawMatches, &rep.rejectedNoRoom);
                            for (size_t k = 0; k < got; ++k)
                            {
                                Hit h{};
                                h.object = const_cast<uint8_t*>(regionBase) + buf[k].offset;
                                h.needle = buf[k].needle;
                                h.regionBase = reinterpret_cast<uintptr_t>(regionBase);
                                h.regionSize = regionSize;
                                out.push_back(h);
                            }
                            rep.bytesScanned += runBytes;
                        }
                        i += run;
                    }
                    offset += pages * kPageSize;
                }
                rep.regionsScanned++;

                // Hand the machine back. See Options::yieldEveryBytes: the
                // thread is not the problem, the memory bandwidth is.
                if (opt.yieldEveryBytes && rep.bytesScanned - lastYieldAt >= opt.yieldEveryBytes)
                {
                    lastYieldAt = rep.bytesScanned;
                    Sleep(opt.yieldMs);
                }

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
