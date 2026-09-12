#include "core/mod.h"

#include <Windows.h>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/settings.h"
#include "core/pinstore.h"
#include "game/alert.h"
#include "game/mapicon.h"
#include "game/realpin.h"
#include "game/player.h"
#include "game/aim.h"
#include "game/actors.h"
#include "game/dump.h"
#include "game/lgso.h"
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
    uint16_t g_chord = 0x00C0;
    uint32_t g_chordHold = 300;
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
        "LevelGimmickSceneObject",
        // Where the game really keeps the player's map markers. Session a
        // hundred and eight read the class name off the object the game
        // itself writes to, and it is not the client component the mod had
        // been using. Finding it in the sweep is what lets a mark be a real
        // marker without the player placing one first.
        "ServerContentsMiscActorComponent"};

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
        "ServerContentsMiscActorComponent",
    };
    uintptr_t g_cameraVt = 0;
    bool g_lookAgainNow = false;
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
        // Six megabytes of our own memory; pausing in the middle of it would
        // only make the self test slower than the thing it is testing.
        opt.yieldEveryBytes = 0;

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

    // The level gimmick database, read straight from the global its own
    // vtable maintains. Dumps the first records whole so their layout can be
    // read rather than guessed at: the reflection tables say they carry a
    // _prefabPath and a _worldTransform.
    void ProbeLevelGimmicks()
    {
        uintptr_t base = 0;
        size_t size = 0;
        if (!gs::typescan::ModuleRange(base, size)) return;
        const uintptr_t at = base + gs::sig::kLgsoManagerGlobal;
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(at), 8))
        {
            GS_LOG("[lgso] the global at +0x%llX is not readable",
                   static_cast<unsigned long long>(gs::sig::kLgsoManagerGlobal));
            return;
        }
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(at);
        if (mgr < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(mgr), 0x80))
        {
            GS_LOG("[lgso] the global holds 0x%p, which is not an object yet",
                   reinterpret_cast<void*>(mgr));
            return;
        }
        const char* cls = gs::rtti::VtableClassName(
            reinterpret_cast<const void*>(*reinterpret_cast<const uintptr_t*>(mgr)));
        const uint32_t count = *reinterpret_cast<const uint32_t*>(mgr + gs::sig::kOff_Lgso_Count);
        const uintptr_t recs = *reinterpret_cast<const uintptr_t*>(mgr + gs::sig::kOff_Lgso_Records);
        GS_LOG_OK("[lgso] manager 0x%p (%s) via the global, %u record(s), array 0x%p",
                  reinterpret_cast<void*>(mgr), cls ? ShortName(cls) : "?", count,
                  reinterpret_cast<void*>(recs));
        if (!count || recs < 0x10000) return;
        if (!gs::rtti::Readable(reinterpret_cast<const void*>(recs), 8ull * (count < 8 ? count : 8))) return;

        const gs::player::Pos pp = gs::player::Read();
        const auto* arr = reinterpret_cast<const uintptr_t*>(recs);

        // The whole table, then the handful nearest the player.
        //
        // Session sixty-six read the names: "Mission_PororinVillage_Bell_All_
        // Calphade", "Hernand_Bell". So these are the game's notable level
        // gimmicks, the kind that earn a map icon, rather than every prop in
        // the world. Whether that includes what glints is the question this
        // answers, and the test is whether anything in here lands near the
        // spot I have been marking, around (-9714, -4141).
        struct Near { float x, y, z, d; uint32_t rec, el; };
        Near best[12]{};
        int bestN = 0;
        uint32_t total = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uintptr_t rec = arr[i];
            if (rec < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(rec), 0x70)) continue;
            for (uintptr_t off = 0; off + 16 <= 0x70; off += 8)
            {
                const uintptr_t a2 = *reinterpret_cast<const uintptr_t*>(rec + off);
                const uint32_t n2 = *reinterpret_cast<const uint32_t*>(rec + off + 8);
                const uint32_t c2 = *reinterpret_cast<const uint32_t*>(rec + off + 12);
                if (a2 < 0x10000 || (a2 & 7) != 0) continue;
                if (n2 == 0 || n2 > c2 || c2 > 100000) continue;
                const size_t span = static_cast<size_t>(n2) * gs::sig::kOff_LgsoData_Stride;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(a2), span)) continue;
                for (uint32_t e = 0; e < n2; ++e)
                {
                    const uintptr_t el = a2 + static_cast<uintptr_t>(e) * gs::sig::kOff_LgsoData_Stride;
                    const float* q = reinterpret_cast<const float*>(el + gs::sig::kOff_LgsoData_Transform);
                    const float unit = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
                    if (!(unit > 0.98f && unit < 1.02f)) continue;   // a wrong read, skipped
                    const float* p = q + 4;
                    if (!(p[0] == p[0]) || !(p[2] == p[2])) continue;
                    ++total;
                    if (!pp.valid) continue;
                    const float dx = p[0] - pp.x, dz = p[2] - pp.z;
                    const float d = std::sqrt(dx * dx + dz * dz);
                    int at = bestN;
                    while (at > 0 && best[at - 1].d > d)
                    {
                        if (at < 12) best[at] = best[at - 1];
                        --at;
                    }
                    if (at < 12) best[at] = Near{p[0], p[1], p[2], d, i, e};
                    if (bestN < 12) ++bestN;
                }
            }
        }
        GS_LOG_OK("[lgso] the table holds %u placement(s) across %u record(s)", total, count);
        if (pp.valid)
        {
            GS_LOG("[lgso] nearest to you at (%.1f, %.1f, %.1f):", pp.x, pp.y, pp.z);
            for (int k = 0; k < bestN; ++k)
                GS_LOG("[lgso]   %6.0f m  record %u element %u at (%.1f, %.1f, %.1f)",
                       best[k].d, best[k].rec, best[k].el, best[k].x, best[k].y, best[k].z);
        }

        for (uint32_t i = 0; i < count && i < 3; ++i)
        {
            const uintptr_t rec = arr[i];
            if (rec < 0x10000 || !gs::rtti::Readable(reinterpret_cast<const void*>(rec), 0x100)) continue;
            char tag[24];
            _snprintf_s(tag, sizeof(tag), _TRUNCATE, "lgso%u", i);
            // A record is 0x70 bytes: session sixty-three's three records sat
            // at 0x...1EA0, 0x...1F10 and 0x...1F80. Only its own bytes are
            // dumped, so the next record is not read as part of it.
            GS_LOG("[lgso] record %u at 0x%p", i, reinterpret_cast<void*>(rec));
            gs::dump::Object(tag, rec, 0x70);

            // The lists it owns, read with the stride and offsets the raw
            // dump gave up. Every element's world position comes out, and its
            // two pointers are dumped on the first one so the prefab path can
            // be found next.
            for (uintptr_t off = 0; off + 16 <= 0x70; off += 8)
            {
                const uintptr_t arr2 = *reinterpret_cast<const uintptr_t*>(rec + off);
                const uint32_t n2 = *reinterpret_cast<const uint32_t*>(rec + off + 8);
                const uint32_t c2 = *reinterpret_cast<const uint32_t*>(rec + off + 12);
                if (arr2 < 0x10000 || (arr2 & 7) != 0) continue;
                if (n2 == 0 || n2 > c2 || c2 > 100000) continue;
                const size_t span = static_cast<size_t>(n2) * gs::sig::kOff_LgsoData_Stride;
                if (!gs::rtti::Readable(reinterpret_cast<const void*>(arr2), span < 0x100 ? 0x100 : span))
                    continue;
                GS_LOG("[lgso] record %u list at +%02llX -> 0x%p, %u of %u", i,
                       static_cast<unsigned long long>(off), reinterpret_cast<void*>(arr2), n2, c2);
                for (uint32_t e = 0; e < n2 && e < 8; ++e)
                {
                    const uintptr_t el = arr2 + static_cast<uintptr_t>(e) * gs::sig::kOff_LgsoData_Stride;
                    const float* q = reinterpret_cast<const float*>(el + gs::sig::kOff_LgsoData_Transform);
                    const float* p = q + 4;
                    const float* s = q + 7;
                    const float unit = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
                    const float dx = p[0] - (pp.valid ? pp.x : 0.0f);
                    const float dz = p[2] - (pp.valid ? pp.z : 0.0f);
                    GS_LOG("[lgso]   %u.%u at (%.3f, %.3f, %.3f) scale %.2f, %.0f m away%s",
                           i, e, p[0], p[1], p[2], s[0], std::sqrt(dx * dx + dz * dz),
                           (unit > 0.98f && unit < 1.02f) ? "" : "  (rotation is not a unit quaternion)");
                    if (e == 0)
                    {
                        char t2[28];
                        _snprintf_s(t2, sizeof(t2), _TRUNCATE, "lgso%u.%02llXe", i,
                                    static_cast<unsigned long long>(off));
                        // Two pointers sit near the head of an element, one into
                        // the image and one on the heap, and the heap one moves
                        // by 0x20 between neighbours. A prefab path is the thing
                        // to hope for.
                        (void)t2;
                        // The pointer at +0x18 leads to string descriptors:
                        // session sixty-five's dump reads a char pointer, a
                        // length of 40, and a hash, repeating every 0x20 bytes.
                        // That is the engine's own string shape, the one the
                        // node names already come out of elsewhere in this mod.
                        for (uintptr_t k = 0; k + 8 <= 0x40; k += 8)
                        {
                            const uintptr_t pv = *reinterpret_cast<const uintptr_t*>(el + k);
                            if (pv < 0x10000 || (pv & 7) != 0) continue;
                            if (!gs::rtti::Readable(reinterpret_cast<const void*>(pv), 0x80)) continue;
                            for (uintptr_t j = 0; j + 16 <= 0x80; j += 0x20)
                            {
                                const uintptr_t cs = *reinterpret_cast<const uintptr_t*>(pv + j);
                                const uint32_t len = *reinterpret_cast<const uint32_t*>(pv + j + 8);
                                if (cs < 0x10000 || len == 0 || len > 240) continue;
                                if (!gs::rtti::Readable(reinterpret_cast<const void*>(cs), len)) continue;
                                char text[248];
                                uint32_t w = 0;
                                bool ok = true;
                                for (; w < len && w + 1 < sizeof(text); ++w)
                                {
                                    const char c = *reinterpret_cast<const volatile char*>(cs + w);
                                    if (c == 0) break;
                                    if (static_cast<unsigned char>(c) < 0x20 ||
                                        static_cast<unsigned char>(c) > 0x7E) { ok = false; break; }
                                    text[w] = c;
                                }
                                text[w] = 0;
                                if (ok && w > 2)
                                    GS_LOG("[lgso]     +%02llX string +%02llX \"%s\"",
                                           static_cast<unsigned long long>(k),
                                           static_cast<unsigned long long>(j), text);
                            }
                        }
                    }
                }
            }
        }
    }

    uintptr_t g_worldVt = 0;
    uintptr_t g_miniVt = 0;
    uintptr_t g_alertVt = 0;

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
            {gs::sig::kAlertRootVtable, gs::sig::kAlertRootClass},
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
            if (vt && e.rva == gs::sig::kAlertRootVtable) g_alertVt = vt;
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
        // Off unless the ini asks. This walk reads five gigabytes and takes
        // eighteen seconds, and it runs again whenever something is still
        // missing, which is the stall I get shortly after the mod starts
        // working. What it hunted were live instances of the detect mode
        // classes, and that route was retired in 0.29.0 when the level gimmick
        // table replaced it. Everything still in use arrives another way: the
        // map roots from the spy and from RTTI, the player from the actor
        // manager, the camera from its own vtable, the table from a fixed
        // global.
        if (!gs::Settings::Get().sweep)
        {
            static bool said = false;
            if (!said) { said = true; GS_LOG("scan: Sweep=0, so the heap is not walked"); }
            return;
        }
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
            const bool isServerMisc =
                strcmp(t.info.name, ".?AVServerContentsMiscActorComponent@pa@@") == 0;
            // One of these is the player's. Several and there is no way to tell
            // which from a vtable alone, so the mod waits to see the game use
            // one instead of guessing.
            if (isServerMisc && found != 1)
                GS_LOG("[real] %zu server marker component(s); the mod will wait to see the game "
                       "use one rather than pick", found);

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
                else if (isServerMisc)
                {
                    if (found == 1) gs::realpin::SetServerSubmodule(t.object);
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
        // Something the mod was holding has been freed, which in practice
        // means a world was thrown away. Whatever the sweep had decided about
        // how long to wait was about that world.
        g_lookAgainNow = true;
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
            // Whatever the ini's Chord names, held for Hold milliseconds.
            if (gs::pad::ChordHeld(g_chord, g_chordHold, 0)) OnTrigger("the pad chord");
            gs::pad::Pump();
            // The entity set, off the game's thread. Twice a second is plenty:
            // an entity stays in the set twelve seconds after it was last seen.
            static uint32_t lastRefresh = 0;
            const uint32_t now = GetTickCount();
            if (now - lastRefresh >= 500)
            {
                lastRefresh = now;
                gs::actors::Refresh(now);
                // Twice a second, and only when the player has stopped
                // answering, which after a load is the whole problem.
                gs::player::Recover();
            }
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
        gs::pinstore::Load(g_self);

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
            gs::mapicon::InstallRemoveSpy(g_worldVt, g_miniVt);
            GS_LOG("spy: %d of 2 slots taken. Open the map and every icon the game creates is logged.", n);
        }
        else
        {
            GS_LOG("spy: off in the ini, the vtables are untouched");
        }
        // The alert system, watched on the same terms. I want a popup
        // saying a glint has been marked; the game already shows popups and
        // this is where they come from. One session of them going past says
        // what a call of our own has to look like.
        if (cfg.spy && g_alertVt) gs::alert::InstallSpy(g_alertVt);
        // And the two marker calls, watched from inside rather than through a
        // vtable, because nothing reaches them through an object.
        // Removable pins need this one, not only the diagnostics: the jump
        // over the server's remove is how the mod hears the map ask for a
        // marker of its own to go.
        if (cfg.spy || cfg.realMarkers) gs::realpin::InstallSpy();
        else if (cfg.spy) GS_LOG_ERR("[alert] the alert root vtable was not found; no popup groundwork this session");
        // The per-frame tick on the game's thread, stacked on the minimap root's
        // update, with the diff probe on for this discovery session.
        gs::tick::InstallWorldMap(g_worldVt);
        if (gs::tick::Install(g_miniVt))
        {
            gs::tick::SetProbe(cfg.verbose);
            GS_LOG("[tick] probe on: root fields that change are logged every 3 s");
        }

        if (g_cameraVt) gs::camera::Install(g_cameraVt);
        else GS_LOG_ERR("PlayerCameraTPSMode vtable not found; no camera this session");
        gs::pad::Init();
        g_key = cfg.key;
        g_chord = cfg.chord;
        g_chordHold = cfg.holdMs;
        g_keyThread = CreateThread(nullptr, 0, KeyThread, nullptr, 0, nullptr);
        GS_LOG("press %s (VK %02X), or hold the pad chord 0x%04X for %lu ms, to mark whatever the "
               "crosshair is on", gs::Settings::KeyName(cfg.key), cfg.key, cfg.chord,
               static_cast<unsigned long>(cfg.holdMs));

        // Early passes hunt for something that may not exist yet, so they come
        // quickly. Once everything is in hand a tick is one pointer read each and
        // the address space is left alone.
        // The essential object first, on its own. One needle over resident
        // memory is a few seconds, and the player, the flash flag and the
        // detect component all hang off it. Session fifteen's presses came
        // before the general sweep reached it.
        // Retried until it lands: the world may still be loading on the first
        // attempt, and a registry entry must never be taken for the player.
        for (int attempt = 0; attempt < 900 && !g_stop.load(); ++attempt)
        {
            bool done = false;

            // The level gimmick table, which is what a press actually reads.
            //
            // It hung off the end of this loop until 0.50.1, which was fine
            // while the loop always finished in forty seconds. With the heap
            // walk switched off the loop can wait minutes, and session
            // ninety-eight is the result: the camera gave a press a perfect
            // position, and the press had nothing to look in. Nothing was
            // marked all session.
            //
            // The table needs a module global and nothing else. It has no
            // business waiting for the player.
            if (gs::lgso::Count() == 0 && gs::lgso::Load() > 0 && gs::Settings::Get().verbose)
            {
                gs::lgso::LogKinds();
                gs::lgso::LogCatalog(40, 64);
            }

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
                                  "origin (%.0f, %.0f, %.0f). Aim blinding flash at a glint.",
                                  static_cast<unsigned long long>(GetTickCount() - g_startedMs),
                                  probe.x, probe.y, probe.z, probe.ox, probe.oy, probe.oz);
                        break;
                    }
                    gs::player::SetSpecialComponent(nullptr);
                }
            }
            // One walk for every candidate vtable, not one walk each.
            //
            // Waiting thirty seconds for the actor manager was the wrong
            // trade and session ninety-five paid for it: the manager offered
            // nine hundred and ninety entities at exactly the thirty second
            // mark and every one of them was without a position, so it could
            // not name the player anyway, and the mod took seventy-nine
            // seconds to come alive instead of thirty.
            //
            // The waste was never the timing. The class has more than one
            // vtable, this loop scanned for them one at a time, and each walk
            // is seventeen seconds over five gigabytes. Two of them ran, found
            // one object each, and the first was somebody else's component.
            // The scanner takes an array of needles and covers them in a single
            // walk, which is what the comment on FindPointers says to do and
            // what the general sweep already does.
            if (!done)
            {
            uintptr_t needles[16]{};
            size_t bytes[16]{};
            size_t slotOf[16]{};
            size_t nn = 0;
            for (size_t i = 0; i < g_count && nn < 16; ++i)
            {
                if (!strstr(g_targets[i].info.name, "ClientSpecialModeActorComponent")) continue;
                if (g_targets[i].object) { done = true; break; }
                needles[nn] = g_targets[i].info.vtableVa;
                bytes[nn] = g_targets[i].objectBytes;
                slotOf[nn] = i;
                ++nn;
            }
            if (nn == 0 || done || !gs::Settings::Get().scan) goto afterFastPass;

            // Not until there is a player to find.
            //
            // Session ninety-nine walked five gigabytes for fourteen seconds
            // and found two special mode components, both of them belonging to
            // a ClientChildOnlyInGameActor rather than the played body, so
            // both were thrown away and the whole walk ran again sixteen
            // seconds later. Thirty seconds of freeze to do one walk's worth
            // of work, because the first one happened before the player was
            // in the world.
            //
            // The camera says when he is, for nothing. It carries his position
            // in world coordinates and the reader only reports that as valid
            // when its two copies agree and land inside the map, which cannot
            // happen before he exists. So the walk waits for it, and when it
            // does run there is a right answer to find.
            {
                const gs::camera::Pose cam = gs::camera::Read();
                if (!cam.valid || !cam.worldValid)
                {
                    static bool said = false;
                    if (!said)
                    {
                        said = true;
                        GS_LOG("the heap walk is waiting for the camera to report a world "
                               "position, which is how it knows the player exists");
                    }
                    goto afterFastPass;
                }
            }
            {
            // The neighbourhood first.
            //
            // The scan is a freeze because it reads five gigabytes. It does not
            // have to start there: the mod already holds live pointers into the
            // game's heap, and objects allocated by the same allocator tend to
            // share arenas. One region is a few tens of megabytes and takes
            // milliseconds, so trying the four we know costs nothing and may
            // save the whole walk.
            const uintptr_t known[] = {
                gs::camera::This(),
                reinterpret_cast<uintptr_t>(gs::mapicon::LastWorldRoot()),
                gs::actors::Manager(),
            };
            for (uintptr_t k : known)
            {
                if (!k || done) continue;
                gs::scan::Options near_;
                near_.needleBytes = bytes;
                near_.timeBudgetMs = 2000;
                near_.onlyRegionContaining = k;
                std::vector<gs::scan::Hit> nearHits;
                gs::scan::FindPointers(needles, nn, nearHits, near_);
                DropPointerTables(nearHits);
                for (const gs::scan::Hit& h : nearHits)
                {
                    if (!h.object) continue;
                    if (h.needle < 0 || static_cast<size_t>(h.needle) >= nn) continue;
                    Target& t = g_targets[slotOf[h.needle]];
                    t.object = h.object;
                    if (!Describe(t)) { t.object = nullptr; continue; }
                    gs::player::SetSpecialComponent(t.object);
                    const gs::player::Pos probe = gs::player::Read();
                    if (!probe.valid || !gs::player::OwnerIsPlayedBody() ||
                        std::fabs(probe.x) + std::fabs(probe.z) <= 1.0f)
                    {
                        gs::player::SetSpecialComponent(nullptr);
                        t.object = nullptr;
                        continue;
                    }
                    GS_LOG_OK("the player's component was in the same region as an object we "
                              "already had, so no walk was needed");
                    gs::tick::AddProbe("special", t.object, 0x400);
                    done = true;
                    break;
                }
            }
            if (done) goto afterFastPass;

            gs::scan::Options opt;
            opt.needleBytes = bytes;
            opt.timeBudgetMs = 20000;
            opt.maxRegionBytes = 1024ull * 1024 * 1024;
            // Session seventeen's fast pass took a 48 KB registry entry for the
            // player's component. Real heap arenas are megabytes.
            opt.minRegionBytes = 1024 * 1024;
            std::vector<gs::scan::Hit> hits;
            const gs::scan::Report rep = gs::scan::FindPointers(needles, nn, hits, opt);
            GS_LOG("fast pass %d: %zu candidate vtable(s), %zu hit(s) in %llu ms", attempt + 1,
                   nn, hits.size(), static_cast<unsigned long long>(rep.microseconds / 1000));
            DropPointerTables(hits);
            for (const gs::scan::Hit& h : hits)
            {
                if (!h.object) continue;
                if (h.needle < 0 || static_cast<size_t>(h.needle) >= nn) continue;
                Target& t = g_targets[slotOf[h.needle]];
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
                              "Aim blinding flash at a glint and press.", pp.x, pp.y, pp.z, pp.ox, pp.oy, pp.oz,
                              gs::actors::Ready() ? "found" : "pending");
                else
                    GS_LOG("special mode component found; player walk %s, detect component %s",
                           pp.valid ? "ok" : "pending", gs::player::DetectComponent() ? "ok" : "pending");
                break;
            }
            }
            }
            afterFastPass:
            if (done) break;
            if (attempt == 0)
                GS_LOG("no world yet. Waiting for the actor manager, which answers once the save "
                       "has finished loading; set Scan=1 in the ini to walk the heap instead and "
                       "be ready sooner at the cost of a freeze.");
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

            static bool lgsoProbed = false;
            if (!lgsoProbed && gs::player::Read().valid)
            {
                lgsoProbed = true;
                ProbeLevelGimmicks();
                if (gs::lgso::Load() > 0 && gs::Settings::Get().verbose)
                {
                    gs::lgso::LogKinds();
                    gs::lgso::LogCatalog(40, 64);
                }
            }

            // How long to wait before the next sweep. A sweep reads gigabytes
            // and takes the better part of a minute, and session fifty ran one
            // every fifteen seconds for eight minutes without finding anything
            // new: the two it wanted were the map roots, which do not exist
            // until the player opens the map, which read as choppiness. So a
            // sweep that finds nothing new buys the next one more time, up to
            // two minutes, and a sweep that finds something starts over.
            static size_t lastLive = 0;
            static int barren = 0;
            int ticks = 30;
            // A world has just been built, so everything the sweep is holding
            // has been thrown away and everything it wants has been made
            // again. Whatever it had decided about how long to wait is about
            // the old world.
            if (g_lookAgainNow || gs::tick::TakeWorldRebuilt())
            {
                g_lookAgainNow = false;
                barren = 0;
                lastLive = 0;
                GS_LOG("something the mod was holding has been freed, so the world is new; "
                       "looking again now rather than on the timer");
            }
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
