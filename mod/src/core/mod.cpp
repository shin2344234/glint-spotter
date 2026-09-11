#include "core/mod.h"

#include <Windows.h>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/settings.h"
#include "game/mapicon.h"
#include "game/player.h"
#include "game/aim.h"
#include "game/actors.h"
#include "game/dump.h"
#include "game/camera.h"
#include "hook/pad.h"
#include "hook/tick.h"
#include "game/rtti.h"
#include "game/scan.h"
#include "game/signatures.h"
#include "game/typescan.h"
#include "version.h"

namespace
{
    std::atomic<bool> g_stop{false};
    HANDLE g_thread = nullptr;
    HANDLE g_keyThread = nullptr;
    uint32_t g_key = 0x91;
    void* g_self = nullptr;

    // The keyword sweep finds 104 classes on build 2.01.00, so a cap of 48 was
    // silently dropping more than half of them.
    constexpr size_t kMaxClasses = 512;
    constexpr size_t kMaxNeedles = 64;

    // A class we are hunting, and the best pointer we have to a live one.
    struct Target
    {
        gs::typescan::ClassInfo info{};
        size_t objectBytes = 0x100;  // fit test; the root controls know better
        void* object = nullptr;
        size_t candidates = 0;
        bool hunt = false;           // false for template instantiations
    };

    Target g_targets[kMaxClasses];
    size_t g_count = 0;

    // The classes the feature depends on. Everything else in the sweep is
    // discovery: named in the log, never hunted.
    const char* const essential[] = {
        "UIGamePlayControlRootWorldMap", "UIGamePlayControlRootMiniMap",
        "ClientSpecialModeActorComponent", "ClientMinimapActorComponent",
        "ClientActorManager",
        // The game's own reflection tables declare
        // pa::LevelGimmickSceneObjectData with _prefabPath and
        // _worldTransform, held in a list on pa::LevelGimmickSceneObjectInfo
        // and managed by pa::LevelGimmickSceneObjectInfoManager, whose vtable
        // sits at RVA 0x056A35D8 on 1.0.0.2850. A database of every level
        // gimmick's prefab and world position is exactly what the feature
        // needs, and nothing else found on disk or in memory carries it.
        //
        // Static analysis could not reach it: that vtable has no code or data
        // reference anywhere in the image, so the object is built through a
        // template whose construction is inlined. That says nothing about
        // whether the object exists at runtime, and the sweep finds objects by
        // their vtable rather than by who points at them, which is how the
        // actor manager was found in the first place.
        "LevelGimmickSceneObject"};

    // Anything in the map icon and detect mode families. Both spellings of
    // minimap appear in this binary, so both are listed.
    // Camera is here for the aim. The pin has to land where the crosshair
    // points, and that needs the view direction, which lives in whatever
    // camera object the game drives. The diff probe finds it as a block of
    // floats that all move when the player turns.
    const char* const kKeywords[] = {
        "MapIcon", "MiniMap", "Minimap", "WorldMap", "DetectMode", "SpecialMode",
        "ClientDetectActorComponent", "ClientTransformSyncActorComponent",
        "ClientActorManager", "PlayerCameraTPSMode", "ClientGimmickActorComponent",
        "LevelGimmickSceneObject", "DiscoveredLevelGimmick",
    };
    uintptr_t g_cameraVt = 0;
    uintptr_t g_managerVt = 0;

    const char* ShortName(const char* decorated)
    {
        // ".?AVFoo@bar@pa@@" reads better as "Foo@bar@pa@@" in a log.
        if (decorated && decorated[0] == '.' && decorated[1] == '?') return decorated + 4;
        return decorated ? decorated : "";
    }

    // The two classes whose size is actually known, from the factory that builds
    // them. A tight fit test is what threw out session one's false positives, so
    // it is worth being explicit about where the number is real and where it is
    // only a floor.
    size_t KnownSize(const char* decorated)
    {
        if (strstr(decorated, "UIGamePlayControlRootWorldMap") ||
            strstr(decorated, "UIGamePlayControlRootMiniMap"))
            return gs::sig::kRootControlSize;
        if (strstr(decorated, "ClientActorManager")) return 0x200;
        return 0x100;
    }

    // Prove the scanner can find a heap object before trusting it to say one is
    // absent. Three sessions came back empty and the interesting question was
    // always whether the game had no such object or the scan could not find one.
    // This settles that in a few milliseconds, using an object we made ourselves.
    struct Canary
    {
        virtual ~Canary() = default;
        virtual int tag() { return 0x6C1A; }
        char filler[0x200]{};
    };

    void SelfTest()
    {
        auto* canary = new Canary();
        const uintptr_t vt = *reinterpret_cast<uintptr_t*>(canary);
        const size_t bytes = sizeof(Canary);

        gs::scan::Options opt;
        opt.needleBytes = &bytes;
        opt.timeBudgetMs = 20000;
        opt.maxHits = 8;
        // Only the region the canary lives in. It proves the same code path
        // in milliseconds; walking everything else proved nothing extra and
        // cost up to twenty seconds a session.
        opt.onlyRegionContaining = reinterpret_cast<uintptr_t>(canary);

        std::vector<gs::scan::Hit> hits;
        const gs::scan::Report rep = gs::scan::FindPointers(&vt, 1, hits, opt);

        bool foundIt = false;
        for (const gs::scan::Hit& h : hits) foundIt |= h.object == canary;

        if (foundIt)
            GS_LOG_OK("self test: found our own object at 0x%p in %llu ms, %llu MB read. "
                      "The scanner works, so an empty result means absence.",
                      static_cast<void*>(canary),
                      static_cast<unsigned long long>(rep.microseconds / 1000),
                      static_cast<unsigned long long>(rep.bytesScanned / (1024 * 1024)));
        else
            GS_LOG_ERR("self test: did NOT find our own object at 0x%p (%zu other hit(s), "
                       "%llu MB read%s). The scanner is broken, not the game.",
                       static_cast<void*>(canary), hits.size(),
                       static_cast<unsigned long long>(rep.bytesScanned / (1024 * 1024)),
                       rep.timeBudgetHit ? ", time budget hit" : "");
        delete canary;
    }

    uintptr_t g_worldVt = 0;
    uintptr_t g_miniVt = 0;

    void Discover()
    {
        uintptr_t base = 0;
        size_t size = 0;
        if (gs::typescan::ModuleRange(base, size))
            GS_LOG("game module 0x%p, %llu MB", reinterpret_cast<void*>(base),
                   static_cast<unsigned long long>(size / (1024 * 1024)));

        gs::typescan::ClassInfo found[kMaxClasses]{};
        const size_t n = gs::typescan::FindClasses(
            kKeywords, sizeof(kKeywords) / sizeof(kKeywords[0]), found, kMaxClasses);

        GS_LOG("RTTI: %zu class(es) in the map icon and detect mode families", n);

        for (size_t i = 0; i < n && g_count < kMaxClasses; ++i)
        {
            if (!found[i].vtableVa)
            {
                // Nothing points at the locator, so the class is abstract or only
                // ever appears inside a template. There is no object to look for.
                GS_LOG("  no vtable            %s", ShortName(found[i].name));
                continue;
            }
            Target& t = g_targets[g_count];
            t.info = found[i];
            t.objectBytes = KnownSize(found[i].name);
            // "?$" marks a template instantiation. Those are binders, pools and
            // reflection glue rather than the objects the create path uses, and
            // there are enough of them to crowd out the real classes. They are
            // still logged, because the argument lists in their names are how the
            // map icon event signature was read in the first place.
            t.hunt = ShortName(found[i].name)[0] != '?';
            // Decorated as PlayerCameraTPSMode@gameClientScript@pa@@; session
            // twenty-two compared against the wrong namespace and had no camera.
            if (strstr(found[i].name, "PlayerCameraTPSMode@"))
            {
                g_cameraVt = found[i].vtableVa;
                t.hunt = false;   // held through its update, not found by scan
            }
            if (strcmp(found[i].name, ".?AVClientActorManager@pa@@") == 0)
            {
                g_managerVt = found[i].vtableVa;
                gs::actors::SetManagerVtable(g_managerVt);
                t.hunt = false;   // found through the globals instead
            }
            // Of the camera family, only objects that are a camera. The events,
            // parameter blocks, presets and descriptors that share the word are
            // data, and session twelve filled every probe slot with them.
            if (t.hunt && strstr(found[i].name, "Camera"))
            {
                static const char* const junk[] = {"FrameEvent", "Param", "Data", "Desc", "Info",
                                                   "Preset", "Query", "Event", "Selector",
                                                   "Shake", "Blend", "Effect", "Sequencer",
                                                   "Volume", "hkx", "Lock", "Rotate", "Pulse"};
                for (const char* j : junk)
                    if (strstr(found[i].name, j)) { t.hunt = false; break; }
            }
            GS_LOG("  %s +0x%08X %3d slots  %s", t.hunt ? "hunt" : "    ",
                   found[i].vtableRva, found[i].slots, ShortName(found[i].name));
            ++g_count;
        }

        // The two addresses static analysis produced, held up against what the
        // running game says. Disagreement means the exe moved and every RVA in
        // signatures.h is stale.
        struct Expect { uintptr_t rva; const char* name; };
        const Expect expected[] = {
            {gs::sig::kWorldMapVtable, gs::sig::kWorldMapClass},
            {gs::sig::kMiniMapVtable,  gs::sig::kMiniMapClass},
        };
        for (const Expect& e : expected)
        {
            // Read the vtable at the expected address and let RTTI name it,
            // independent of what the keyword sweep happened to keep.
            const bool agreed = gs::rtti::VtableIs(reinterpret_cast<const void*>(base + e.rva), e.name);
            uintptr_t vt = agreed ? base + e.rva : 0;
            if (agreed)
                GS_LOG_OK("signatures.h +0x%08llX still matches %s",
                          static_cast<unsigned long long>(e.rva), ShortName(e.name));
            else
            {
                // The 11 September patch moved every address. The sweep names
                // the same class by RTTI, and that vtable is as good.
                for (size_t i = 0; i < n && !vt; ++i)
                    if (found[i].vtableVa && strcmp(found[i].name, e.name) == 0) vt = found[i].vtableVa;
                if (vt)
                    GS_LOG_OK("signatures.h +0x%08llX is stale; %s found by RTTI at +0x%08llX instead",
                              static_cast<unsigned long long>(e.rva), ShortName(e.name),
                              static_cast<unsigned long long>(vt - base));
                else
                    GS_LOG_ERR("signatures.h +0x%08llX no longer matches %s and RTTI did not offer it",
                               static_cast<unsigned long long>(e.rva), ShortName(e.name));
            }
            // Only a vtable the running game has just named gets hooked.
            if (vt && e.rva == gs::sig::kWorldMapVtable) g_worldVt = vt;
            if (vt && e.rva == gs::sig::kMiniMapVtable)  g_miniVt = vt;
        }

        // The gimmick component's vtable, so the entity set can find the
        // component with one compare and read its detect mode target byte.
        {
            uintptr_t gvt = gs::rtti::VtableIs(reinterpret_cast<const void*>(base + gs::sig::kGimmickVtable), gs::sig::kGimmickClass)
                                ? base + gs::sig::kGimmickVtable : 0;
            for (size_t i = 0; i < n && !gvt; ++i)
                if (found[i].vtableVa && strcmp(found[i].name, gs::sig::kGimmickClass) == 0) gvt = found[i].vtableVa;
            if (gvt)
            {
                gs::actors::SetGimmickVtable(gvt);
                GS_LOG_OK("ClientGimmickActorComponent vtable at +0x%08llX (%s); glint byte at +0x%llX",
                          static_cast<unsigned long long>(gvt - base),
                          gvt == base + gs::sig::kGimmickVtable ? "as recorded" : "by RTTI, the record is stale",
                          static_cast<unsigned long long>(gs::sig::kOff_Gimmick_DetectTgt));
            }
            else GS_LOG_ERR("ClientGimmickActorComponent not found by address or RTTI; gimmicks found by name, no glint byte");
        }
    }

    // Read what the create path would need. False means the candidate does not
    // stand up and the caller must drop it: session one cached a rejected
    // pointer, the next tick confirmed its vtable, and no scan ever ran again.
    // Every read of a candidate, in one leaf with no C++ objects so it can sit
    // inside __try. Session five crashed to desktop here: map icons are built and
    // freed each time the map opens, Readable said yes, and the read that
    // followed landed on memory the game had just released. The pre-check stays
    // because it is cheap, but only a handler around the read itself is a fix.
    bool Snapshot(const void* obj, size_t objectBytes, bool wantSlots,
                  uint64_t* hdr, void** slot35, void** slot170)
    {
        __try
        {
            if (!gs::rtti::Readable(obj, objectBytes)) return false;
            const auto* q = static_cast<const uint64_t*>(obj);
            hdr[0] = q[0];
            hdr[1] = q[1];
            hdr[2] = q[2];
            *slot35 = nullptr;
            *slot170 = nullptr;
            if (wantSlots)
            {
                auto** vt = *reinterpret_cast<void***>(const_cast<void*>(obj));
                const size_t want = static_cast<size_t>(gs::sig::kSlotCreateIcon + 1) * sizeof(void*);
                if (gs::rtti::Readable(vt, want))
                {
                    *slot35 = vt[gs::sig::kSlotUpdate];
                    *slot170 = vt[gs::sig::kSlotCreateIcon];
                }
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool Describe(Target& t)
    {
        // Slot 170 only means "create icon" on the two root controls. Other
        // classes have vtables of their own length, and the slot counter walks
        // straight past the end into the next vtable, so printing it for them
        // was noise.
        const bool root = strstr(t.info.name, "UIGamePlayControlRoot") != nullptr;

        uint64_t hdr[3]{};
        void* s35 = nullptr;
        void* s170 = nullptr;
        if (!Snapshot(t.object, t.objectBytes, root, hdr, &s35, &s170))
        {
            GS_LOG("    not readable for a whole object, dropped");
            return false;
        }

        // In a table of vtable pointers the neighbouring qword is another
        // vtable, and in an object it is member data. Session two found 112
        // candidates and every one was a table entry, so this test is what
        // separates the two.
        if (gs::rtti::VtableClassName(reinterpret_cast<const void*>(hdr[1])))
        {
            GS_LOG("    +08 is itself a vtable, so this is a pointer table, dropped");
            return false;
        }

        if (root && s35)
            GS_LOG("    slot %d update 0x%p, slot %d create icon 0x%p",
                   gs::sig::kSlotUpdate, s35, gs::sig::kSlotCreateIcon, s170);

        // First bytes of the object. Cheap, and it is what will later tell us
        // whether two candidates are one control seen twice or two controls.
        GS_LOG("    +00 %016llX  +08 %016llX  +10 %016llX",
               static_cast<unsigned long long>(hdr[0]),
               static_cast<unsigned long long>(hdr[1]),
               static_cast<unsigned long long>(hdr[2]));
        return true;
    }

    void LogReport(const char* label, const gs::scan::Report& rep)
    {
        GS_LOG("%s: %zu region(s) read, %llu MB, %llu ms%s", label, rep.regionsScanned,
               static_cast<unsigned long long>(rep.bytesScanned / (1024 * 1024)),
               static_cast<unsigned long long>(rep.microseconds / 1000),
               rep.timeBudgetHit ? ", TIME BUDGET HIT, coverage incomplete" : "");
        GS_LOG("  skipped %zu wrong kind, %zu too small, %zu too large (%llu MB), "
               "%llu MB not resident",
               rep.regionsSkippedKind, rep.regionsSkippedSmall, rep.regionsSkippedLarge,
               static_cast<unsigned long long>(rep.bytesSkippedLarge / (1024 * 1024)),
               static_cast<unsigned long long>(rep.bytesSkippedNotResident / (1024 * 1024)));
        GS_LOG("  %zu pointer match(es), %zu rejected for having no room behind them",
               rep.rawMatches, rep.rejectedNoRoom);
    }

    // A region that answers to several different classes at once is a table of
    // vtable pointers rather than a heap. One 48 KB region in session two held
    // hits for all 56 classes and every one of them was noise. Returns how many
    // hits are left standing.
    size_t DropPointerTables(std::vector<gs::scan::Hit>& hits)
    {
        // Session five threw out two regions for answering to five classes, and a
        // heap arena with five kinds of object in it is exactly what a heap looks
        // like. The registry answers to all 56 at once. Sixteen tells them apart,
        // and the neighbour test in Describe catches anything smaller.
        constexpr size_t kTableThreshold = 16;
        for (size_t a = 0; a < hits.size(); ++a)
        {
            if (!hits[a].object) continue;
            int seen[kMaxNeedles]{};
            size_t distinct = 0;
            for (size_t b = a; b < hits.size(); ++b)
            {
                if (!hits[b].object || hits[b].regionBase != hits[a].regionBase) continue;
                bool already = false;
                for (size_t k = 0; k < distinct; ++k)
                    if (seen[k] == hits[b].needle) already = true;
                if (!already && distinct < kMaxNeedles) seen[distinct++] = hits[b].needle;
            }
            if (distinct < kTableThreshold) continue;

            size_t dropped = 0;
            const uintptr_t region = hits[a].regionBase;
            for (gs::scan::Hit& h : hits)
                if (h.regionBase == region && h.object) { h.object = nullptr; ++dropped; }
            GS_LOG("  region 0x%llX answers to %zu different classes, so it is a table "
                   "of vtable pointers; %zu hit(s) discarded",
                   static_cast<unsigned long long>(region), distinct, dropped);
        }

        size_t live = 0;
        for (const gs::scan::Hit& h : hits) live += h.object ? 1 : 0;
        return live;
    }

    // One walk covering every class with no live pointer yet. Narrow first
    // because it is fast, then wide in the same tick if everything came back
    // empty, because a session costs real time and coming back with nothing is
    // the expensive outcome.
    void ScanFor()
    {
        uintptr_t needles[kMaxClasses]{};
        size_t bytes[kMaxClasses]{};
        size_t slotOf[kMaxClasses]{};
        size_t n = 0;
        // Two passes: the classes the feature depends on first, then everything
        // else that fits. Session twelve lost the map roots to the needle cap
        // because a hundred camera classes sort ahead of them by address.
        // Only the essentials are hunted. The other fifty classes were
        // discovery, and scanning for them cost fifteen seconds in every twenty
        // for the whole session.
        for (int pass = 0; pass < 1; ++pass)
        {
            for (size_t i = 0; i < g_count; ++i)
            {
                if (!g_targets[i].hunt || g_targets[i].object) continue;
                bool ess = false;
                for (const char* e : essential) if (strstr(g_targets[i].info.name, e)) ess = true;
                if ((pass == 0) != ess) continue;
                bool already = false;
                for (size_t k = 0; k < n; ++k) if (slotOf[k] == i) already = true;
                if (already) continue;
                if (n >= kMaxNeedles) break;
                needles[n] = g_targets[i].info.vtableVa;
                bytes[n] = g_targets[i].objectBytes;
                slotOf[n] = i;
                ++n;
            }
        }
        if (n == 0) return;

        gs::scan::Options opt;
        opt.needleBytes = bytes;
        // Session two spent its four seconds on 509 MB and stopped there, so most
        // of the heap was never looked at. This runs only while something is
        // still missing, so a long pass costs a pause and not a stutter.
        opt.timeBudgetMs = 25000;
        // Session twenty-two: a 132 KB registry region offered a "world root"
        // whose +08 was a vtable RVA and a name string, and slot 170 on it
        // took the game down. Objects live in the big heap regions.
        opt.minRegionBytes = 1024 * 1024;
        // Session six skipped one 275 MB region as too large. With residency
        // filtering the size cap buys little, so it is generous now.
        opt.maxRegionBytes = 1024ull * 1024 * 1024;

        std::vector<gs::scan::Hit> hits;
        gs::scan::Report rep = gs::scan::FindPointers(needles, n, hits, opt);
        LogReport("narrow scan", rep);

        // Tables have to be thrown out before deciding whether to widen, or a
        // hundred table entries read as success and the wide pass never runs.
        if (DropPointerTables(hits) == 0)
        {
            hits.clear();
            opt.wideKinds = true;
            opt.residentOnly = false;
            opt.timeBudgetMs = 60000;
            opt.maxRegionBytes = 4ull * 1024 * 1024 * 1024;
            rep = gs::scan::FindPointers(needles, n, hits, opt);
            LogReport("wide scan", rep);
            DropPointerTables(hits);
        }

        for (size_t i = 0; i < n; ++i)
        {
            Target& t = g_targets[slotOf[i]];
            size_t found = 0;
            for (const gs::scan::Hit& h : hits)
                if (h.object && h.needle == static_cast<int>(i)) ++found;
            if (found == 0) continue;

            t.candidates = found;
            GS_LOG_OK("%s: %zu candidate(s)", ShortName(t.info.name), found);
            // The two objects the tick wants: the world root to place pins on,
            // and the player's special mode component to watch for the flash.
            const bool isWorldRoot = strstr(t.info.name, "UIGamePlayControlRootWorldMap") != nullptr;
            const bool isSpecial = strstr(t.info.name, "ClientSpecialModeActorComponent") != nullptr;
            const bool isCamera = strstr(t.info.name, "Camera") != nullptr &&
                                  ShortName(t.info.name)[0] != '?';
            const bool isManager = strcmp(t.info.name, ".?AVClientActorManager@pa@@") == 0;
            const bool isLevelGimmick = strstr(t.info.name, "LevelGimmickSceneObject") != nullptr;

            size_t shown = 0;
            for (const gs::scan::Hit& h : hits)
            {
                if (!h.object || h.needle != static_cast<int>(i)) continue;
                if (shown++ >= 6) break;
                GS_LOG("  [%zu] 0x%p in region 0x%llX +0x%llX", shown - 1, h.object,
                       static_cast<unsigned long long>(h.regionBase),
                       static_cast<unsigned long long>(h.regionSize));
                t.object = h.object;
                if (!Describe(t)) t.object = nullptr;
                else if (isWorldRoot) gs::tick::SetWorldRoot(t.object);
                else if (isSpecial)
                {
                    gs::tick::AddProbe("special", t.object, 0x400);
                    gs::player::SetSpecialComponent(t.object);
                }
                else if (isLevelGimmick)
                {
                    // Whatever this turns out to be, the useful part is what it
                    // points at. LevelGimmickSceneObjectInfo is supposed to own
                    // a _levelGimmickSceneObjectDataList, and a list of
                    // prefab-and-transform records is what to look for: a
                    // pointer to an array, or a count beside one.
                    GS_LOG("  [levelgimmick] %s at 0x%p", ShortName(t.info.name), t.object);
                    const gs::player::Pos lp = gs::player::Read();
                    gs::dump::Vectors("lgsovec", reinterpret_cast<uintptr_t>(t.object), 0x400,
                                      lp.valid ? lp.x : 0.0f, lp.valid ? lp.z : 0.0f);
                    gs::dump::Pointers("lgso", reinterpret_cast<uintptr_t>(t.object), 0x200);
                    gs::dump::Object("lgsohex", reinterpret_cast<uintptr_t>(t.object), 0x200);
                }
                else if (isManager)
                {
                    // A heap hit for the manager is not trusted: session nineteen
                    // found a registry entry. The vtable is what the globals
                    // finder needs, and it was set at discovery.
                }
                else if (isCamera)
                {
                    // Not probed by the player any more. The view direction is
                    // found from the binary offline; the aim comes from the
                    // detect component instead.
                }
            }
            if (found > shown) GS_LOG("  ... %zu more", found - shown);
        }
    }

    bool Recheck(Target& t)
    {
        if (!t.object) return false;
        if (gs::scan::StillValid(t.object, t.info.vtableVa)) return true;
        GS_LOG("%s: 0x%p stopped carrying its vtable, will look again",
               ShortName(t.info.name), t.object);
        gs::tick::DropProbe(t.object);
        if (strstr(t.info.name, "ClientSpecialModeActorComponent")) gs::player::SetSpecialComponent(nullptr);
        t.object = nullptr;
        return false;
    }

    // What the hotkey does in this build: nothing to the game. It reports what
    // the spy has seen and what a replay would pass, so the key path and the
    // captured data can both be checked before a call is ever made.
    // The trigger. Position is the best one known at this stage: the last pin
    // the player placed this session, offset 40 units, so the whole chain from
    // trigger to tick to pin is proven while the probe is still finding the
    // live position. Falls back to the player marker, which is stale but real.
    void OnTrigger(const char* how)
    {
        GS_LOG("[trigger] %s. ticks so far %llu on thread %lu", how,
               static_cast<unsigned long long>(gs::tick::Count()), gs::tick::ThreadId());

        if (gs::tick::Count() == 0)
        {
            GS_LOG("[trigger] the tick has never run, so there is no game thread to place from");
            return;
        }
        gs::tick::RequestMark(0.0f, 0.0f, "GlintSpotter");
    }

    // The CRT answers an invalid parameter by calling __fastfail, which kills the
    // whole process. That is a defensible default for an application and a
    // terrible one for a plugin living in someone else's: opening the log in the
    // CRT's Unicode mode made the first narrow fprintf invalid, and the game died
    // at startup with STATUS_STACK_BUFFER_OVERRUN and no log to say why.
    //
    // The static CRT means this handler is ours alone and the game's own is
    // untouched. Returning from it makes the offending call fail and set errno
    // instead of ending the process.
    thread_local bool t_inHandler = false;

    void __cdecl OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                                    unsigned int, uintptr_t)
    {
        if (t_inHandler) return;
        t_inHandler = true;
        GS_LOG_ERR("CRT invalid parameter swallowed; a call failed but the process lives");
        t_inHandler = false;
    }

    // __DATE__ and __TIME__ bake in when mod.cpp last compiled, and mod.cpp only
    // recompiles when its own dependencies change, so the banner goes stale while
    // the rest of the plugin moves. Master Looter's notes call this out after it
    // cost a test round there. Report what the file on disk says instead.
    void LogBuildStamp(void* selfModule)
    {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(static_cast<HMODULE>(selfModule), path, MAX_PATH)) return;

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;

        SYSTEMTIME utc{}, local{};
        FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);

        GS_LOG("plugin file written %04d-%02d-%02d %02d:%02d:%02d, %lu bytes",
               local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
               local.wSecond, fad.nFileSizeLow);
    }

    // 50 ms steps so a press is not missed, edge-detected so a held key fires
    // once. On its own thread because the worker spends up to 25 seconds inside
    // a scan, and session seven's press landed in one and was never seen.
    DWORD WINAPI KeyThread(LPVOID)
    {
        bool wasDown = false;
        while (!g_stop.load())
        {
            const bool down = (GetAsyncKeyState(static_cast<int>(g_key)) & 0x8000) != 0;
            if (down && !wasDown) OnTrigger("key");
            wasDown = down;
            // RB + LB + A. XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100, RIGHT_SHOULDER 0x0200, A 0x1000.
            if (gs::pad::ChordPressed(0x0100 | 0x0200 | 0x1000)) OnTrigger("RB+LB+A");
            // The entity set, off the game's thread. Twice a second is plenty:
            // an entity stays in the set twelve seconds after it was last seen.
            static uint32_t lastRefresh = 0;
            const uint32_t now = GetTickCount();
            if (now - lastRefresh >= 500) { lastRefresh = now; gs::actors::Refresh(now); }
            Sleep(50);
        }
        return 0;
    }

    uint32_t g_startedMs = 0;

    DWORD WorkerBody()
    {
        g_startedMs = GetTickCount();
        GS_LOG("Glint Spotter %s probe", GS_VERSION_STRING);
        LogBuildStamp(g_self);
        GS_LOG("Read-only. It reads the game's own RTTI, looks for those objects in");
        GS_LOG("memory, and writes what it finds. No hooks, nothing called.");

        // The exe is mapped long before the UI exists, so RTTI can be read early
        // even though nothing has been built from it yet.
        const gs::Settings::Values& cfg = gs::Settings::Load(g_self);

        Sleep(1000);
        SelfTest();
        Discover();
        if (g_count == 0)
        {
            GS_LOG_ERR("no classes with vtables found, nothing to look for this session");
            return 0;
        }

        // The first write to the game: one pointer in each of two vtables, only
        // after the running exe has named both classes itself.
        if (cfg.spy)
        {
            const int n = gs::mapicon::InstallSpy(g_worldVt, g_miniVt);
            GS_LOG("spy: %d of 2 slots taken. Open the map and every icon the game creates is logged.", n);
        }
        else
        {
            GS_LOG("spy: off in the ini, the vtables are untouched");
        }
        // The per-frame tick on the game's thread, stacked on the minimap root's
        // update, with the diff probe on for this discovery session.
        if (gs::tick::Install(g_miniVt))
        {
            gs::tick::SetProbe(true);
            GS_LOG("[tick] probe on: root fields that change are logged every 3 s");
        }

        if (g_cameraVt) gs::camera::Install(g_cameraVt);
        else GS_LOG_ERR("PlayerCameraTPSMode vtable not found; no camera this session");
        gs::pad::Init();
        g_key = cfg.key;
        g_keyThread = CreateThread(nullptr, 0, KeyThread, nullptr, 0, nullptr);
        GS_LOG("press %s (VK %02X) or RB+LB+A with the flash aimed at a glint to pin the glint", gs::Settings::KeyName(cfg.key), cfg.key);

        // Early passes hunt for something that may not exist yet, so they come
        // quickly. Once everything is in hand a tick is one pointer read each and
        // the address space is left alone.
        // The essential object first, on its own. One needle over resident
        // memory is a few seconds, and the player, the flash flag and the
        // detect component all hang off it. Session fifteen's presses came
        // before the general sweep reached it.
        // Retried until it lands: the world may still be loading on the first
        // attempt, and a registry entry must never be taken for the player.
        for (int attempt = 0; attempt < 40 && !g_stop.load(); ++attempt)
        {
            bool done = false;

            // The manager knows where the player is, and it costs nothing to
            // ask. Session forty-seven spent forty-seven seconds before the
            // first pin was possible, nearly all of it in two heap scans of
            // fourteen seconds each, looking for an object the manager was
            // already handing over. The scan below stays as the fallback.
            gs::actors::Locate(GetTickCount());
            if (gs::actors::Ready())
            {
                uintptr_t special = 0;
                const uintptr_t ent = gs::actors::PlayerEntity(&special);
                if (ent && special)
                {
                    gs::player::SetSpecialComponent(reinterpret_cast<void*>(special));
                    const gs::player::Pos probe = gs::player::Read();
                    if (probe.valid && std::fabs(probe.x) + std::fabs(probe.z) > 1.0f)
                    {
                        done = true;
                        gs::tick::AddProbe("special", reinterpret_cast<void*>(special), 0x400);
                        GS_LOG_OK("READY in %llu ms: the manager handed over the player at (%.1f, %.1f, %.1f), "
                                  "origin (%.0f, %.0f, %.0f). Aim the flash at a glint.",
                                  static_cast<unsigned long long>(GetTickCount() - g_startedMs),
                                  probe.x, probe.y, probe.z, probe.ox, probe.oy, probe.oz);
                        break;
                    }
                    gs::player::SetSpecialComponent(nullptr);
                }
            }
            for (size_t i = 0; i < g_count && !done; ++i)
            {
            Target& t = g_targets[i];
            if (!strstr(t.info.name, "ClientSpecialModeActorComponent")) continue;
            if (t.object) { done = true; break; }
            const uintptr_t needle = t.info.vtableVa;
            const size_t bytes = t.objectBytes;
            gs::scan::Options opt;
            opt.needleBytes = &bytes;
            opt.timeBudgetMs = 20000;
            opt.maxRegionBytes = 1024ull * 1024 * 1024;
            // Session seventeen's fast pass took a 48 KB registry entry for the
            // player's component. Real heap arenas are megabytes.
            opt.minRegionBytes = 1024 * 1024;
            std::vector<gs::scan::Hit> hits;
            const gs::scan::Report rep = gs::scan::FindPointers(&needle, 1, hits, opt);
            GS_LOG("fast pass %d for the special mode component: %zu hit(s) in %llu ms", attempt + 1,
                   hits.size(), static_cast<unsigned long long>(rep.microseconds / 1000));
            DropPointerTables(hits);
            for (const gs::scan::Hit& h : hits)
            {
                if (!h.object) continue;
                t.object = h.object;
                if (!Describe(t)) { t.object = nullptr; continue; }
                // Every character has one of these. The player's is the one
                // whose owner is the played body. A player standing at the
                // origin is the menu's placeholder, not the world's.
                gs::player::SetSpecialComponent(t.object);
                const gs::player::Pos probe = gs::player::Read();
                if (!probe.valid || !gs::player::OwnerIsPlayedBody() ||
                    std::fabs(probe.x) + std::fabs(probe.z) <= 1.0f)
                {
                    GS_LOG("  0x%p is a special mode component but not the player's, skipped", t.object);
                    gs::player::SetSpecialComponent(nullptr);
                    t.object = nullptr;
                    continue;
                }
                done = true;
                gs::tick::AddProbe("special", t.object, 0x400);
                // Walk to the player and the detect component right now rather
                // than on the next probe sample, and say READY only when both
                // are in hand, because the press needs both.
                const gs::player::Pos pp = gs::player::Read();
                gs::actors::Locate(GetTickCount());
                if (pp.valid && gs::player::DetectComponent() && gs::player::CharacterControlComponent())
                    GS_LOG_OK("READY: player world (%.1f, %.1f, %.1f), origin (%.0f, %.0f, %.0f), actor manager %s. "
                              "Aim the flash at a glint and press.", pp.x, pp.y, pp.z, pp.ox, pp.oy, pp.oz,
                              gs::actors::Ready() ? "found" : "pending");
                else
                    GS_LOG("special mode component found; player walk %s, detect component %s",
                           pp.valid ? "ok" : "pending", gs::player::DetectComponent() ? "ok" : "pending");
                break;
            }
            }
            if (done) break;
            GS_LOG("no world yet, looking again in a second");
            for (int i = 0; i < 2 && !g_stop.load(); ++i) Sleep(500);
        }

        int pass = 0;
        int idle = 0;
        while (!g_stop.load())
        {
            size_t live = 0, hunted = 0;
            for (size_t i = 0; i < g_count; ++i)
            {
                if (!g_targets[i].hunt) continue;
                bool ess = false;
                for (const char* e : essential) if (strstr(g_targets[i].info.name, e)) ess = true;
                if (!ess) continue;
                ++hunted;
                live += Recheck(g_targets[i]) ? 1 : 0;
            }

            // The two objects the aim depends on, once the player walk has
            // named them. They change during ordinary play, so no ritual.
            static bool watchingAim = false;
            if (!watchingAim && gs::player::DetectComponent())
            {
                watchingAim = true;
                gs::tick::AddProbe("detect", reinterpret_cast<void*>(gs::player::DetectComponent()), 0x400);
            }

            gs::actors::Locate(GetTickCount());

            // How long to wait before the next sweep. A sweep reads gigabytes
            // and takes the better part of a minute, and session fifty ran one
            // every fifteen seconds for eight minutes without finding anything
            // new: the two it wanted were the map roots, which do not exist
            // until the player opens the map. Seth saw that as choppiness. So a
            // sweep that finds nothing new buys the next one more time, up to
            // two minutes, and a sweep that finds something starts over.
            static size_t lastLive = 0;
            static int barren = 0;
            int ticks = 30;
            if (live < hunted)
            {
                if (live > lastLive) barren = 0;
                else if (barren < 4) ++barren;
                lastLive = live;
                GS_LOG("--- pass %d, %zu of %zu located ---", ++pass, live, hunted);
                ScanFor();
                idle = 0;
                static const int kWait[5] = {10, 30, 60, 120, 240};  // 5 s to 2 min
                ticks = kWait[barren];
            }
            else if (++idle == 1)
            {
                GS_LOG_OK("all %zu located, holding. Nothing more unless one changes.", hunted);
            }
            for (int i = 0; i < ticks && !g_stop.load(); ++i) Sleep(500);
        }
        return 0;
    }

    // The thread entry holds no C++ objects, so it can carry the handler that
    // WorkerBody cannot. Whatever the guards above miss ends the probe for the
    // session and writes why, instead of taking the game with it.
    DWORD WINAPI Worker(LPVOID)
    {
        __try
        {
            return WorkerBody();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GS_LOG_ERR("unhandled exception 0x%08lX on the probe thread; probe stopped, game lives",
                       static_cast<unsigned long>(GetExceptionCode()));
            return 1;
        }
    }
}

namespace gs::Mod
{
    void Initialize(void* selfModule)
    {
        _set_invalid_parameter_handler(OnInvalidParameter);
        g_self = selfModule;
        Log::Start(selfModule);
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }

    void Shutdown(bool processTerminating)
    {
        g_stop.store(true);
        // On process teardown the loader lock is held and other threads are
        // already gone, so waiting on one is how a plugin hangs an exit.
        if (!processTerminating && g_thread) WaitForSingleObject(g_thread, 3000);
        if (!processTerminating && g_keyThread) WaitForSingleObject(g_keyThread, 1000);
        if (g_keyThread) { CloseHandle(g_keyThread); g_keyThread = nullptr; }
        // The vtable slots go back only when the process is staying up. On
        // teardown the game is leaving anyway, and a write to its memory from
        // inside DllMain buys nothing.
        if (!processTerminating) { gs::tick::Remove(); gs::mapicon::RemoveSpy(); }
        if (g_thread)
        {
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
        Log::Shutdown();
    }
}
