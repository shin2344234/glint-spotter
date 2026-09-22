#pragma once
#include <cstdint>

// Everything read out of CrimsonDesert.exe. First on 10 September 2026 from
// exe 1.0.0.2760; re-derived on 11 September from 1.0.0.2850 after the game
// patched itself overnight and every address below moved at once; and again
// on 17 September from 1.0.0.2944, which also grew some structs by 8 bytes.
//
// A game patch moves all of it. Nothing below is dereferenced until a runtime
// check agrees: a vtable must name its class through RTTI, a function must
// start with the bytes the analysis saw, or the value is found again at
// runtime by RTTI or by byte pattern. Re-derive the constants with
// docs/investigations/rebase.py, which prints this block for a new exe.
//
// 2949, the 21 September patch, put sixteen bytes into the code somewhere
// between 0x495700 and 0xD77700. Everything above that moved by exactly 0x10
// and everything below it, the two client pin functions included, did not.
// Each address below was checked by reading its recorded prologue out of the
// new exe rather than by assuming the shift. Vtables, the physics facade, the
// frame offset and the level gimmick manager's global did not move at all.
//
// The full write-up is in FEASIBILITY.md.

namespace gs::sig
{
    constexpr const char* kExeVersion = "1.0.0.2949";

    // The two root UI controls that own every map icon. Slot 35 is the per frame
    // update, slot 170 creates an icon. The vtables are also found by RTTI at
    // runtime when these are stale.
    constexpr uintptr_t kWorldMapVtable = 0x0566FE78;
    constexpr uintptr_t kMiniMapVtable  = 0x05674408;

    constexpr const char* kWorldMapClass = ".?AVUIGamePlayControlRootWorldMap@uiCommonScript@pa@@";
    constexpr const char* kMiniMapClass  = ".?AVUIGamePlayControlRootMiniMap@uiCommonScript@pa@@";

    constexpr int kSlotUpdate     = 35;   // void __fastcall(this, float) on the world map
    constexpr int kSlotCreateIcon = 170;  // the map icon dispatcher

    // And the slot next door, which takes one away.
    //
    // Found by following what the mod's own pins are made of. Slot 171 is
    // 3476 bytes at RVA 0x00D308F0 and it calls 0x00D2FED0, which takes the
    // root and a key, finds that key's bucket in the icon map at root+0x3B0,
    // and moves the bucket's contents out from under it. Its prologue homes
    // the same three arguments slot 170's does, in the same order:
    //
    //   mov dword ptr [rsp + 0x20], r9d   ; the fourth argument, a dword
    //   mov qword ptr [rsp + 0x18], r8    ; the key, read as sixteen bytes
    //   mov word ptr [rsp + 0x10], dx     ; the icon type, by value here
    //
    // So removing a pin is the same three values the mod already passes to
    // create one, and the mod knows every key it has ever used.
    //
    // On 2944 slot 171 is 0x00DB8AE0, 3476 bytes again with the same prologue,
    // but the bucket lookup is inlined into it: it reads its own table off the
    // root at +0x198 onwards, and root+0x3B0 is now a list the update walks.
    constexpr int kSlotRemoveIcon = 171;
    constexpr uintptr_t kRemoveIconBody = 0x00DB8AF0;

    // Who asked for a removal. Two functions call the same slot and only
    // one of them is the player pressing delete: the icon builder removes
    // an icon before it replaces one of the same key, and the remover next
    // door is the delete itself. Session a hundred and five caught both a
    // second apart, from +0x00D66C24 and +0x00D778EB.
    //
    // A record only; no code reads these. They are still the 2850 values. On
    // 2944 slot 171 has thirteen callers instead of two, and 0x00DD3540 is only
    // the likeliest remover by shape. A spy log of a real delete settles it.
    constexpr uintptr_t kIconRemoverLo = 0x00D77710;
    constexpr uintptr_t kIconRemoverHi = 0x00D77942;

    // The alert system root, which owns every on-screen message the game
    // shows: toasts, region changes, item pickups, level ups.
    //
    // Slot 144 is the one that matters. It is 974 bytes at RVA 0x00F36100
    // (0x00FBE3B0 on 2944, the same three strings) and
    // it names three strings outright, "Toast", "BountyHunter" and
    // "TimerGauge", comparing each against a string it pulls out of its fourth
    // argument. That argument is a list: a tag byte, a data pointer at +8, a
    // count at +0x10, and entries of 0x18 bytes, walked until one has 0x0F in
    // its first byte. So it is the entry point that turns "show an alert
    // called X" into an actual widget, and a call with "Toast" in that list is
    // a popup.
    //
    // What the list looks like filled in is what this build is for. The spy
    // forwards every call unchanged and writes down what went past, which is
    // exactly how slot 170 and the map pin were solved in sessions seven to
    // ten.
    constexpr uintptr_t kAlertRootVtable = 0x056CA2A0;
    constexpr const char* kAlertRootClass = ".?AVUIGamePlayControlRootAlertSystem@uiCommonScript@pa@@";
    constexpr int kSlotAlertCall = 144;   // of 168

    // The game's own marker list, and the two functions that change it.
    //
    // I could not delete the mod's pins, and the reason is that they were
    // never markers. The mod builds a UI icon directly on the map root, which
    // draws correctly and belongs to nothing, so the game's own delete has
    // nothing to delete. His hand-placed markers go somewhere else first: a
    // submodule hanging off the player that keeps one growable list per icon
    // kind, and the map is drawn from that.
    //
    // The chain, from re-pinmarker-flow.md, traced through the Ack that
    // commits a marker the server confirmed:
    //   submodule = *(*(actor + 0x68) + 0x168)
    //   the list for a kind is at submodule + 0xC8 + kind * 16
    //   a record is 24 bytes: int64 id, float x, y, z, then two flag bytes
    // Which list holds them was wrong here for four builds. 0x15 is real, but
    // it is the icon key's kind and a fixed tag in a notification, not an
    // index into this submodule: the only code that ever creates a marker
    // reaches list 0 or list 1 and no other, and 0x15 fails the range checks
    // that same function applies to its own inputs. List 0 is a marker, list 1
    // is a traced marker, and list 0x15 was empty every session because
    // nothing writes to it.
    // The client copy's writer, and the one function in this whole feature
    // that was documented correctly from the first pass. The acknowledgement
    // calls it to put the server's answer into the client component, which is
    // the copy the map's own UI reads, so a marker the mod creates has to go
    // through here as well or the UI never learns it exists.
    //
    //   0x4260C0(this = client submodule, int32* outError, uint8 kind,
    //            struct { const void* records; int32 count; }*)
    //
    // Find or insert by id, so calling it twice with one id is an update.
    constexpr uintptr_t kPinUpsert = 0x00495460;   // (submodule, &err, kind, &{records, count})
    constexpr uint8_t   kPinUpsertPrologue[12] = {
        0x44, 0x88, 0x44, 0x24, 0x18,   // mov [rsp+0x18], r8b   the kind
        0x48, 0x89, 0x54, 0x24, 0x10,   // mov [rsp+0x10], rdx   the error out
        0x53, 0x55,                     // push rbx, rbp
    };
    // And the client copy's eraser, which is what the acknowledgement calls
    // when the server agrees a marker is gone. Read off its own prologue:
    //
    //   0x426360(this = client submodule, int32* outError, const int64* id,
    //            uint8 kind)
    //
    // Not found leaves a sentinel and writes an error, and changes nothing.
    constexpr uintptr_t kPinRemove = 0x00495700;   // (submodule, &err, &id, kind)
    constexpr uint8_t   kPinRemovePrologue2[15] = {
        0x48, 0x89, 0x5C, 0x24, 0x08,   // mov [rsp+0x08], rbx
        0x48, 0x89, 0x6C, 0x24, 0x10,   // mov [rsp+0x10], rbp
        0x48, 0x89, 0x74, 0x24, 0x18,   // mov [rsp+0x18], rsi
    };

    // Ids the mod gives its own markers. Far above anything the game hands
    // out, which is a small index into a bitmap, so the two can never be
    // confused and a request carrying one of these is the map asking the mod
    // to take a pin away.
    constexpr int64_t   kModIdBase = 0x40000000;
    constexpr uintptr_t kOff_Actor_Components = 0x68;
    constexpr uintptr_t kOff_Comp_PinSubmodule = 0x168;
    constexpr uintptr_t kOff_Pin_Lists         = 0xC8;
    constexpr int       kPinKind               = 0x15;  // the icon key's kind
    constexpr int       kPinListKind           = 0;     // the list a marker lives in
    constexpr size_t    kPinRecord             = 24;

    // And the two calls that write it, which is what makes a marker real.
    //
    // Create takes the submodule, a dead second argument, the position, two
    // style bytes by pointer, and a byte choosing list 0 or list 1. It range
    // checks the style bytes, evicts the oldest record when the list is full,
    // assigns the id itself, appends the record and publishes the change. The
    // remove is the same shape with an id instead of a position. Both are
    // reached from the in-process wire and from nowhere else, which is why
    // neither has a caller a search of the image can find.
    //
    // The prologues below are checked against the running game before either
    // address is called. A patched exe fails the check and the mod goes back
    // to drawing pins it cannot delete.
    //
    // On 2944 all four of these functions were found again the way they were
    // first found: the TrocTr*PinMarker* classes by RTTI, each one's slot 2
    // deserializer, and the call it makes. The four deserializers and the
    // create, upsert and erase came out the same size to the byte as on 2850,
    // every prologue matched, and none of the offsets in this chain moved.
    constexpr uintptr_t kPinCreate       = 0x028C3640;
    constexpr uintptr_t kPinServerRemove = 0x028C3B10;
    constexpr uint8_t   kPinCreatePrologue[15] = {
        0x4C, 0x89, 0x4C, 0x24, 0x20,   // mov [rsp+0x20], r9
        0x4C, 0x89, 0x44, 0x24, 0x18,   // mov [rsp+0x18], r8
        0x48, 0x89, 0x54, 0x24, 0x10,   // mov [rsp+0x10], rdx
    };
    constexpr uint8_t   kPinRemovePrologue[10] = {
        0x44, 0x88, 0x44, 0x24, 0x18,   // mov [rsp+0x18], r8b
        0x48, 0x89, 0x54, 0x24, 0x10,   // mov [rsp+0x10], rdx
    };
    // Clear and the create keeps fifteen markers, set and it keeps five
    // hundred. Read only, and only so the log can say which cap applies.
    constexpr uintptr_t kPinOnlineFlag   = 0x06CE0888;
    constexpr uintptr_t kOff_Pin_Owner   = 0x08;   // submodule + 8, the notify target

    // And the map's end of it: what the world map does when it is told a
    // marker exists.
    //
    // The map subscribes two small thunks to two events on the UI model,
    // 0xD79280 for an added marker and 0xD792C0 for a removed one, and each
    // forwards to a real handler. The add handler is this one. It is a
    // thousand bytes and the icon dispatcher is one call inside it, which is
    // exactly what the mod was skipping by calling the dispatcher itself: the
    // icon appeared and nothing else the map keeps about a marker existed.
    //
    // Its arguments come off the dispatcher's own direct-call path, and the
    // prologue agrees with every one of them:
    //
    //   rcx = the world map root      (mov rdi, rcx)
    //   rdx = the record's id, int64  (mov r15, rdx)
    //   r8  = the position, twelve bytes (mov r14, r8)
    //   r9b = the record's byte at +0x14 (movzx esi, r9b)
    //   arg5 = the byte at +0x15, arg6 = the byte at +0x16
    //
    // Unconfirmed on 2944. 0x00DC4420 has this prologue and calls slot 170 once
    // without removing first, but it takes the id as a word and the fourth
    // argument whole, which is not what 2850 did. Nothing calls this address,
    // and it no longer gates the marker calls: the prologue is shared by
    // hundreds of functions, so it never proved much.
    constexpr uintptr_t kMarkerAdded = 0x00DC4430;
    constexpr uint8_t   kMarkerAddedPrologue[16] = {
        0x48, 0x89, 0x5C, 0x24, 0x18,   // mov [rsp+0x18], rbx
        0x55, 0x56, 0x57,               // push rbp, rsi, rdi
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,  // push r12..r15
    };

    // Size the factory allocates for a root control (0xC48 on 2760); a sanity
    // bound. 2944's factory at 0x00EC5AB0 asks for 0xC58.
    constexpr size_t kRootControlSize = 0xC58;

    // The third person camera mode: sixteen slots, slot 2 is Update(this, dt).
    // Slot 2's function is checked by its first bytes, not its address: the
    // update starts by reading the fade weight at this+0x338 and that read is
    // unique in the image.
    constexpr uintptr_t kCameraTPSVtable   = 0x057003E0;
    constexpr const char* kCameraTPSClass  = ".?AVPlayerCameraTPSMode@gameClientScript@pa@@";
    constexpr int       kSlotCameraUpdate  = 2;
    constexpr uintptr_t kCameraTPSUpdate   = 0x011CA550;
    constexpr uint8_t   kCameraUpdatePrologue[22] = {
        0x48, 0x8B, 0xC4,                         // mov rax, rsp
        0x48, 0x89, 0x58, 0x08,                   // mov [rax+8], rbx
        0x57,                                     // push rdi
        0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, // sub rsp, 0xD0
        0xC5, 0xFA, 0x10, 0x81, 0x40, 0x03, 0x00, // vmovss xmm0, [rcx+0x340]
    };
    // Where that read's displacement starts in the bytes above. The check skips
    // it: 2944 moved the field from +0x338 to +0x340 and changed nothing else.
    constexpr size_t    kCameraUpdateFieldAt = 19;
    constexpr uintptr_t kOff_Cam_Pivot     = 0x30;   // float3, then packed cell indices
    constexpr uintptr_t kOff_Cam_Quat      = 0x40;   // x, y, z, w
    constexpr uintptr_t kOff_Cam_Distance  = 0x50;

    // The gimmick component on world objects, and the byte the detect mode
    // event handler sets on it (slot 124 stores it at +0x45B, slot 7 reads it).
    // 2944 moved it to +0x463: slot 124, 0x008FD920, now writes
    // mov byte ptr [rbp+0x463], r15b with rbp the component.
    // pa::LevelGimmickSceneObjectInfoManager is a singleton, and it keeps its
// own pointer in a module global. Slots 2 and 3 of its vtable are the setter
// and the clear, and both write the same address:
//
//   RVA 0x0154FAC0  mov qword ptr [rip + 0x56E2DF9], rcx ; ret
//   RVA 0x0154FAD0  mov qword ptr [rip + 0x56E2DE5], 0   ; ret
//
// Both resolve to RVA 0x06C328C0. On 2944 the slots are 0x015E28E0 and
// 0x015E28F0 and both write 0x06D6E428. The lookup at RVA 0x00433370 then says how
// the records are reached:
//
//   mov edi, dword ptr [rcx]           the key, a plain index
//   mov rbx, qword ptr [rip+0x67FF535] the manager, the same global
//   cmp edi, dword ptr [rbx + 8]       against the count
//   lea rsi, [rdi*8]
//   mov rax, qword ptr [rbx + 0x58]    an array of record pointers
//   mov rax, qword ptr [rsi + rax]     array[key]
//
// Session sixty-two read 171 at manager+0x08 and a pointer at +0x58, which
// matches. This beats the heap sweep: one read of a fixed global instead of
// gigabytes of scanning.
constexpr uintptr_t kLgsoManagerGlobal = 0x06D6E428;

// Globals that hold the ClientActorManager. The startup scan for them reads the
// whole image a pointer at a time, and while a world is loading that took 42 to
// 72 seconds on 22 September and four and a half minutes in hawkeye69's 1.1.24
// log, so the ready buzz came a minute or more after the world. Every 2944 and
// 2949 log found it at one of these two. They are checked first, against the
// manager's vtable, and the scan only runs if neither holds one.
constexpr uintptr_t kActorManagerGlobals[2] = {0x06D69A38, 0x06DDA900};
constexpr uintptr_t kOff_Lgso_Count    = 0x08;
constexpr uintptr_t kOff_Lgso_Records  = 0x58;

// A record's lists hold the placements. Session sixty-four's raw dump of
// record 0's list at +0x20 gives the layout outright:
//
//   +0x64  quaternion  (0.0000, 0.7292, 0.0000, 0.6843)   a unit rotation
//   +0x74  position    (-11896.2109, 713.9378, -2027.1711)
//   +0x80  scale       (1.0, 1.0, 1.0)
//
// and the next element repeats it at +0x12C, +0x13C, +0x148, so the stride is
// 0xC8. That is the same forty-byte Transform the .palevel files carry,
// quaternion then position then scale, sitting at +0x64 of a 200-byte record.
//
// The positions are real and far: (-11896, 714, -2027) and (-4442, 410,
// -3879) with the player near (-9711, 569, -4260), and they sit beside the
// gimmick map icons the game creates for itself out there.
//
// 2944 put sixteen bytes in front of the Transform. Seth's first session on
// it read record 0's first element as position (0, 0.729, 0), scale 0.68,
// which is that same quaternion, and the table came out as 1385 placements
// where 2850 had 17728, so no press and no flash found anything. The game's
// own walkers now step by 0xD8 (0x004985D0: add r14, 0xd8) and read the
// position at +0x84, +0x88, +0x8C, which puts the Transform at +0x74.
constexpr uintptr_t kOff_LgsoData_Stride    = 0xD8;
constexpr uintptr_t kOff_LgsoData_Transform = 0x74;
constexpr uintptr_t kOff_Transform_Pos      = 0x10;   // after the quaternion

constexpr uintptr_t kGimmickVtable         = 0x055B7800;
    constexpr const char* kGimmickClass        = ".?AVClientGimmickActorComponent@pa@@";
    constexpr uintptr_t kOff_Comps_Gimmick     = 0x30;
    constexpr uintptr_t kOff_Gimmick_DetectTgt = 0x463;

    // The reveal itself. ClientDetectActorComponent (block slot +0x50, on
    // characters and on anything else the flash can reveal) keeps a byte at
    // +0x1DA that a single toggle (0x8FBF30 on 2850) sets when it spawns the
    // DetectEffect on the actor's effect component and clears when it
    // removes it. Gimmicks without a detect component keep their reveal as
    // a list of active custom render values on the sub-object at gimmick
    // +0x438: count at +0x1B8, pushed and popped by the DetectLighting
    // handler. Static findings on 2850, checked by a second pass; live
    // confirmation is the next session's job.
    //
    // On 2944 the sub-object pointer moved to +0x440 (gimmick slot 123,
    // 0x008ED4A0, and the DetectLighting body at 0x09B3D7E0 both read it
    // there); its count at +0x1B8 did not move, and neither did the detect
    // component's +0x1DA (the toggle is 0x00976630, 796 bytes as before).
    // The gimmick's base constructor at 0x0E6412A0 leaves +0x3D0..+0x3D7
    // unwritten, the only hole in that range and the size of the shift, so
    // every gimmick field from +0x3D0 up moved by 8 and nothing below it did.
    constexpr const char* kDetectClass          = ".?AVClientDetectActorComponent@pa@@";
    constexpr uintptr_t kOff_Comps_Detect       = 0x50;
    constexpr uintptr_t kOff_Detect_Lit         = 0x1DA;
    constexpr uintptr_t kOff_Gimmick_Sub        = 0x440;
    constexpr uintptr_t kOff_GimmickSub_Active  = 0x1B8;

    // What makes a gimmick a thing the player can pick up, from Master
    // Looter, which has read these since build 2474: item data hangs at
    // +0xC0, gather data at +0xE0, and either one means the node yields
    // something. The node's own name is an engine string at +0x68 and the
    // locked byte at +0x3E2. These are what the blinding flash reveals.
    constexpr uintptr_t kOff_Gimmick_ItemData   = 0xC0;
    constexpr uintptr_t kOff_Gimmick_GatherData = 0xE0;
    constexpr uintptr_t kOff_Gimmick_NodeName   = 0x68;
    // Locked moved with the rest past +0x3D0, by that constructor's hole
    // rather than by an instruction that reads it: no code found reads this
    // byte directly. It only decides whether the log says "locked".
    constexpr uintptr_t kOff_Gimmick_Locked     = 0x3EA;

    // Knowledge. The reveal effect the flash plays is named for it
    // (fx_detectmode_knowledge_gimmick), and the audit traced
    // ClientKnowledgeActorComponent to block slot +0x150 (slot index 0x2A).
    // Session 36: the gimmick under the crosshair had no item or gather
    // data, which is what an object that yields something only after a
    // puzzle looks like, so knowledge is the better test.
    constexpr uintptr_t kOff_Comps_Knowledge    = 0x150;

    // The node's prefab path, which is what Master Looter identifies a node
    // by and what its log was still printing correctly on 2850 while this
    // mod saw nothing: "/object/cd_gimmick/00_common/item/gimmick_item_
    // trade_salt_02.prefab". Two routes to the same string; the first is the
    // one every node answered on in Master Looter's 6 September probe.
    constexpr uintptr_t kOff_Gimmick_Prefab     = 0x18;
    constexpr uintptr_t kOff_Prefab_Path        = 0x38;
    constexpr uintptr_t kOff_Gimmick_PrefabAlt  = 0x58;
    constexpr uintptr_t kOff_PrefabAlt_Path     = 0x18;

    // The game's ray cast wrapper (start, direction, distance in; hit distance
    // and normal out) and what it uses. Found at runtime by byte pattern, with
    // the facade and the frame offset decoded from its own RIP-relative
    // operands; these are the fallback and the record.
    constexpr uintptr_t kRayCastWrapper   = 0x03A10370;
    constexpr uintptr_t kPhysicsFacade    = 0x06A50FF0;
    constexpr uintptr_t kPhysicsFrameOff  = 0x06D56520;
    constexpr int       kSlotCastRay      = 61;   // still 61 on 2944, by TtWorldCastRay's one xref
}
