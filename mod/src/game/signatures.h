#pragma once
#include <cstdint>

// Everything read out of CrimsonDesert.exe. First on 10 September 2026 from
// exe 1.0.0.2760; re-derived on 11 September from 1.0.0.2850 after the game
// patched itself overnight and every address below moved at once.
//
// A game patch moves all of it. Nothing below is dereferenced until a runtime
// check agrees: a vtable must name its class through RTTI, a function must
// start with the bytes the analysis saw, or the value is found again at
// runtime by RTTI or by byte pattern. Re-derive the constants with
// docs/investigations/rebase.py, which prints this block for a new exe.
//
// The full write-up is in FEASIBILITY.md.

namespace gs::sig
{
    constexpr const char* kExeVersion = "1.0.0.2850";

    // The two root UI controls that own every map icon. Slot 35 is the per frame
    // update, slot 170 creates an icon. The vtables are also found by RTTI at
    // runtime when these are stale.
    constexpr uintptr_t kWorldMapVtable = 0x0555E1A8;
    constexpr uintptr_t kMiniMapVtable  = 0x05561F58;

    constexpr const char* kWorldMapClass = ".?AVUIGamePlayControlRootWorldMap@uiCommonScript@pa@@";
    constexpr const char* kMiniMapClass  = ".?AVUIGamePlayControlRootMiniMap@uiCommonScript@pa@@";

    constexpr int kSlotUpdate     = 35;   // void __fastcall(this, float) on the world map
    constexpr int kSlotCreateIcon = 170;  // the map icon dispatcher

    // Size the factory allocates for a root control (2760); a sanity bound.
    constexpr size_t kRootControlSize = 0xC48;

    // The third person camera mode: sixteen slots, slot 2 is Update(this, dt).
    // Slot 2's function is checked by its first bytes, not its address: the
    // update starts by reading the fade weight at this+0x338 and that read is
    // unique in the image.
    constexpr uintptr_t kCameraTPSVtable   = 0x055F3BB0;
    constexpr const char* kCameraTPSClass  = ".?AVPlayerCameraTPSMode@gameClientScript@pa@@";
    constexpr int       kSlotCameraUpdate  = 2;
    constexpr uintptr_t kCameraTPSUpdate   = 0x0113D2B0;
    constexpr uint8_t   kCameraUpdatePrologue[22] = {
        0x48, 0x8B, 0xC4,                         // mov rax, rsp
        0x48, 0x89, 0x58, 0x08,                   // mov [rax+8], rbx
        0x57,                                     // push rdi
        0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, // sub rsp, 0xD0
        0xC5, 0xFA, 0x10, 0x81, 0x38, 0x03, 0x00, // vmovss xmm0, [rcx+0x338]
    };
    constexpr uintptr_t kOff_Cam_Pivot     = 0x30;   // float3, then packed cell indices
    constexpr uintptr_t kOff_Cam_Quat      = 0x40;   // x, y, z, w
    constexpr uintptr_t kOff_Cam_Distance  = 0x50;

    // The gimmick component on world objects, and the byte the detect mode
    // event handler sets on it (slot 124 stores it at +0x45B, slot 7 reads it).
    constexpr uintptr_t kGimmickVtable         = 0x054A8A58;
    constexpr const char* kGimmickClass        = ".?AVClientGimmickActorComponent@pa@@";
    constexpr uintptr_t kOff_Comps_Gimmick     = 0x30;
    constexpr uintptr_t kOff_Gimmick_DetectTgt = 0x45B;

    // The reveal itself. ClientDetectActorComponent (block slot +0x50, on
    // characters and on anything else the flash can reveal) keeps a byte at
    // +0x1DA that a single toggle (0x8FBF30 on 2850) sets when it spawns the
    // DetectEffect on the actor's effect component and clears when it
    // removes it. Gimmicks without a detect component keep their reveal as
    // a list of active custom render values on the sub-object at gimmick
    // +0x438: count at +0x1B8, pushed and popped by the DetectLighting
    // handler. Static findings on 2850, checked by a second pass; live
    // confirmation is the next session's job.
    constexpr const char* kDetectClass          = ".?AVClientDetectActorComponent@pa@@";
    constexpr uintptr_t kOff_Comps_Detect       = 0x50;
    constexpr uintptr_t kOff_Detect_Lit         = 0x1DA;
    constexpr uintptr_t kOff_Gimmick_Sub        = 0x438;
    constexpr uintptr_t kOff_GimmickSub_Active  = 0x1B8;

    // The game's ray cast wrapper (start, direction, distance in; hit distance
    // and normal out) and what it uses. Found at runtime by byte pattern, with
    // the facade and the frame offset decoded from its own RIP-relative
    // operands; these are the fallback and the record.
    constexpr uintptr_t kRayCastWrapper   = 0x03928920;
    constexpr uintptr_t kPhysicsFacade    = 0x06919FB8;
    constexpr uintptr_t kPhysicsFrameOff  = 0x06C1AE10;
    constexpr int       kSlotCastRay      = 61;
}
