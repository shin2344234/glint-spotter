# Glint spotter: can it be built

Research done 10 September 2026, before any code. The idea: use the blinding
flash to reveal glinting objects, and while the flash is held, drop a map marker
where the glint is.

Everything below is checked against files on this machine or against a URL, not
against memory. Where something is unproven it says so.

References to `private/` are working notes kept off the repository, mostly
per-session write-ups and captures of another mod's behaviour on this machine.
Nothing in this document depends on reading them.

## Bottom line

All three pieces are reachable, and nobody has built this. The marker piece looked
blocked on the first pass and is not: the engine has a named map icon creation
path and a map icon key reserved for the detect effect. See the update at the
bottom, dated the same day, which supersedes the pessimistic reading in the
section on piece three.

A mod-drawn pin over the map and the minimap remains the safe fallback, and the
world-to-map projection it needs is already a solved problem. What another mod on
this machine has solved, and how that was read, is in `private/CRIMSON-ROUTE.md`,
which stays local.

## Piece one: knowing which objects glint

Solvable, and better documented than expected.

Blinding flash and the game's internal Detect Mode are one system. The proof is
shipped English text in `master looter/extracted/0020/gamedata/ui.paloc`:

> Using Blinding Flash or lighting the lantern while wearing the helm reveals
> nearby targets whose knowledge you have not yet acquired.

The mode table `extracted/0008/gamedata/specialmode.staticinfobody` carries nine
Detect keys, with per-character variants that matter because of the Damiane
split already documented in DAMIANE.md:

```
Detect                         DetectMode              DetectTaeguk
Detect_Damian                  Detect_Oongka           DetectTaeguk_Damian
Detect_Lantern                 DetectTaeguk_Oongka     Detect_InteractionAim_NoLantern
```

The highlight itself is a named effect family in `effectinfo.staticinfobody`:

```
fx_detectmode_knowledge_gimmick        fx_detectmode_knowledge_gimmick_fail
fx_detectmode_knowledge_character      fx_detectmode_knowledge_character_halo
fx_detectmode_knowledge_effect         fx_detectmode_enemy_hp
fx_detectmode_quest_target_character
```

`fx_detectmode_knowledge_gimmick` is the one that matters. A gimmick is exactly
what MasterLooter's `Fill()` already reads identity from, so a glinting object is
an entity the existing scan can already see and name. There is also a `_fail`
variant, which suggests the game evaluates a per-object test and shows a
different effect when it fails. The reveal is data driven rather than a blanket
shader, and that is what the design hung on.

Two rows in `conditioninfo.staticinfobody` call `IsStageDetectModeTarget()`.
Read those, never write them: FINDINGS.md line 1108 records that patching
`conditioninfo`'s `parserType` from 3 to 0 kills the game at the title screen.

One thing to fix before reusing the scan. `mod/src/loot/engine.cpp:1874` already
throws away most of what would glint:

```cpp
if (IStr(c.node, "visione") || IStr(c.node, "quest") || IStr(c.node, "artifact")) return skip("quest or memory trigger");
if (IStr(c.node, "abyssruins")) return skip("fast-travel artifact");
```

That is deliberate for a looting mod and wrong for this one. Glint candidates
need their own path out of `Fill()`, not a relaxed skip list.

## Piece two: knowing the flash is held

Mostly done. Two routes, and the second is the better one.

Raw input works today. `mod/src/gui/menu.cpp:260` reads `GetAsyncKeyState` in
`KeyDown` and polls per frame in `PollToggle`, gated on
`State::ForegroundIsOurs()` rather than on the menu being open, so it does not
eat the game's input. On the pad, `xinput_hook.cpp:157` exposes a button bitmask
in `PadButtons`, but the only test built on it, `PadChordHeld`, bails at
`PopCount(mask) < 2` on line 181 and cannot see a single held shoulder button.
That is one function to add.

Guessed keybinds break when the player rebinds, so prefer reading the game's own
state. `ui.paloc` names the action ids: `Key_Skill_8_Start` for the concentrate
light stance, `Key_Skill_5_Start` for the Helm of Knowledge. Better still, the
special mode component has its own update function, and that is the component
family behind `Detect` and `Detect_Lantern`. The hook point is proven installable
and proven live on this build, with the evidence in `private/CRIMSON-ROUTE.md`.
What it carries in its arguments is untested.

## Piece three: putting a pin on the map

This is the piece with the least good news.

No native write turned up at first. The most serious public work on this game,
[blizz3010/CrimsonDesertCoop](https://github.com/blizz3010/CrimsonDesertCoop),
76 commits of verified offsets, documents a read-only hook at
`CrimsonDesert.exe+0xAB5594` that pulls an outgoing waypoint out of
`[r15+0x1C..0x28]`, and says the apply side has not been identified. Nothing
found on this machine since contradicts that.

The projection is solved, though, and that is the finding that changes the plan.
A live world-to-map transform runs here today, with map-open detection and a
live player position beside it, holding to under half a pixel against the
player's own dot. The parameters and where they were read are in
`private/CRIMSON-ROUTE.md`. Whatever else is unknown, drawing a pin at the right
place on the map is not.

One thing the UI layer is not: scriptable. A byte search of the exe finds zero
hits for CEF, Chromium, WebView2, CoherentUI, CoherentGT, V8 or Lua, against 216
hits for Pearl Abyss and 64 for BlackSpace. The `.html`, `.css` and `.thtml`
files in the archive are a proprietary markup format with no scripting engine
behind them, so the Black Desert trick of injecting JavaScript and calling the
map function from inside the UI does not exist here. One mod's internal naming
for its own mechanism suggests otherwise and is misleading; see
`private/CRIMSON-ROUTE.md`.

## What to build

Inside MasterLooter, not as a second binary. It already has the worker loop, the
present hook, the entity scan and the input plumbing, and a second `.asi`
fighting for the same render slots is a problem nobody needs.

- `loot/engine.cpp`, beside `Fill()`: a glint candidate path that tests a scanned
  entity's gimmick node against the Detect target families, and does not route
  through the skip at line 1874.
- `hooks/xinput_hook.cpp`: a single-button held test next to `PadChordHeld`.
- A prototype hook on the special mode component's update, installed the way
  `private/CRIMSON-ROUTE.md` describes, logging its arguments through
  `events.cpp`'s existing `SpyEnqueue` spy.
- `hooks/dx12_hook.cpp`, in the existing overlay compositor: draw the pin, using
  a world-to-map transform taken off the map's own update call.

## Fallbacks, ranked

1. Mod-drawn pin on the world map and the minimap. Everything above minus the
   native call. It does not survive the map closing and it does not reach the
   compass or the quest tracker. Say that on the mod page rather than letting
   people find out.
2. No pin at all. Show bearing and distance to the nearest glint candidate and
   let the player place the vanilla pin by hand. This needs only piece one, which
   is the piece most likely to work.
3. Find the native write. Correct long term, last here because nobody has, and
   the way in is to instrument a manual pin placement and catch the write that
   feeds the `+0xAB5594` read.

Driving another mod's local API was on this list and came off it on 10 September
2026. I ruled out integrating with the navigation mod and am not involving
its author, so everything here stands on its own hooks, and where a slot is
shared the plugin stacks on it the way Master Looter already does.

## Experiments, cheapest first

**Does `special_mode_component_update` carry a readable Detect flag?** Hook it,
log its arguments on entry, then use the blinding flash. Look for a small integer
that changes on use and matches a `specialmode` key.

**Is `IsStageDetectModeTarget()` a per-entity runtime bit or a mission flag?**
Take one of the two `conditioninfo` rows, find the gimmick prefab it names, and
watch it through `SpyEnqueue` while standing near it with the flash active. A
state change on the entity settles it.

**Does the game raise an event when a glint appears?** `hkEnqueue` already spies
on every event. Use the flash next to a known knowledge target with the spy log
wide open and see whether anything fires that does not fire otherwise. This is
the cheapest of the three and would make piece one nearly free.

**Does a native marker write exist?** Place a pin by hand through the world map
while logging writes near whatever the `+0xAB5594` read points at.

**Does MasterLooter collide on the map hooks?** Another mod already owns both
map update slots on this machine. The present-hook ordering problem in Master
Looter's notes is the same shape, and the answer there was to stack rather than
wrap. Assume the
same applies and test it early. Details in `private/CRIMSON-ROUTE.md`.

## Reasons not to build it

If the real goal is finding abyss artifacts and lore treasure, the identification
half already exists.
[Always Blinding Flash Vision REBORN](https://www.nexusmods.com/crimsondesert/mods/2131)
by Kriosym keeps the glow on permanently behind a toggle, and the
[original](https://www.nexusmods.com/crimsondesert/mods/1004) by JasonChiuCC did
the same before it. Neither needs any of the work above.

What this idea adds over those is the marker, and the marker is the piece with no
proven native path and a render-slot argument with a popular mod attached to it.
Worth knowing that before starting, not after.

## Corrections to the first research pass

Three findings from the first pass were wrong and were traced and fixed. All
three were read out of another mod's logs and config, so they are recorded in
`private/CRIMSON-ROUTE.md` rather than here.

## Prior art: nothing does this

Checked 10 September 2026 across Nexus, GitHub, trainer and cheat table sites,
Steam and Reddit threads, and Chinese and Korean modding forums. Every claimed mod
page was opened to confirm it exists and does what the listing says.

Nothing connects a detect mode reveal to a map marker. What exists splits cleanly
in two, and neither half crosses over.

Mods that reveal and never mark:

- [Always Blinding Flash Vision REBORN](https://www.nexusmods.com/crimsondesert/mods/2131),
  Kriosym, v5.6, 5 May 2026. Permanent glow on an END toggle, survives fast travel
  and reloads. Eleven tracked versions, no marker feature in any of them.
- [Always Blinding Flash Vision](https://www.nexusmods.com/crimsondesert/mods/1004),
  JasonChiuCC, the predecessor.
- [Crimson Desert ESP](https://www.nexusmods.com/crimsondesert/mods/3277), Jumpman2.
  World-space boxes over live actors and static chests. Never touches the map, and
  does not cover ore, herbs, knowledge points or lore artifacts.

Mods that mark and never reveal:

- [Crimson Route](https://www.nexusmods.com/crimsondesert/mods/3175), dofo7777,
  v6.9.6, 5 September 2026. Routes to a marker that already exists.
- [Crimson Atlas](https://www.nexusmods.com/crimsondesert/mods/3381), SaykiKun. A
  companion map with 70,000 locations and hand-placed personal markers.
- [DesertLink](https://www.nexusmods.com/crimsondesert/mods/3444) and
  [Crimson Navigation](https://www.nexusmods.com/crimsondesert/mods/3447),
  both uploaded 9 September 2026. Saved waypoints and routing to the game's own
  Set Destination.
- [Map Marker Teleport](https://www.nexusmods.com/crimsondesert/mods/3235),
  EmpressAlae, and its fork
  [WayPoint Teleport](https://www.nexusmods.com/crimsondesert/mods/3416),
  UnLuckyLust. Teleport to whatever marker is already active.

Every one of those either consumes a marker the game already tracks or one a human
placed by clicking. None creates a marker from something the game revealed.

Players have asked for it. A Steam thread titled "Setting Map Markers when using
Blinding Flash/Lantern?", posted by Brown Suga Bread on 27 March 2026, asks for
"a way to set map markers so you can mark all the special points of interest while
using the abilities so you can go back to them later"
([thread](https://steamcommunity.com/app/3321460/discussions/0/805720975111944289/),
fetched and confirmed). One reply agrees. Nobody suggests a mod, because there is
not one. Two further threads with the same ask turned up in search and were not
opened, so treat those as reported rather than checked.

Game patches through 1.18, 15 August 2026, added marker shape and colour choices
and map fog filtering. None of them added marker creation from an ability.

## Update: the marker write is not missing, it was mis-scoped

Same day, after the prior-art sweep. The first pass looked for a waypoint struct
to write and did not find one, and concluded the marker was blocked. The engine
does not model this as a waypoint struct at all. It models it as a map icon on an
actor, and the machinery is named in the exe.

The game's quest scripting has a function for exactly this feature. Its reflection
field list, read out of `CrimsonDesert.exe` at `0x5678E80`:

```
StageChart_Function_UIShowMinimap   Base Class StageChart_Function_WithActor
  _isDetectModeTarget    _isEnable        _showPath
  _isTriggerLine         _hideConditionString    _uiMapTextureInfo
```

It takes an actor, turns its minimap presence on, flags it as a detect mode
target, optionally draws a path to it, and picks its icon from
`uimaptextureinfo`. The game's own quest scripts already do what this mod wants
to do, through one built-in call.

The runtime side is named too:

```
AUCreateMapIconInfo        AUCreateMapIconCompareInfo
MapIconCreateRequestFrom   ->  WorldMap, MiniMap
MapIconPositionKey         AVCachedMapIconInfo_updateTask
LevelGimmickMapIconUpdated AVTrocTrLevelGimmickMapIconUpdatedAck
```

`AUCreateMapIconInfo` is a creation path, not a reader. `LevelGimmickMapIconUpdated`
says gimmick map icons already update at runtime, and a glinting object is a
gimmick. Of the 25 `MapIcon_` keys in the binary, two settle the intent:

```
MapIcon_DetectEffect        MapIcon_Pin_Marker
```

The game ships a map icon key for the detect effect. Whether it is wired to
anything today is unknown, and that is now the first thing to test.

So the marker work is not a blind hunt for a write site. It is finding
`AUCreateMapIconInfo`, learning its arguments, and calling it with a gimmick the
scan already identified and `MapIcon_DetectEffect` as the icon. If that call
works, the pin is native: it survives the map closing and it reaches the minimap
without the mod drawing anything.

Also confirmed by the same sweep, from NattKh's data-table tooling and checked
against the exe: `SpecialModeInfo` carries a `DetectModeAreaData` block with
`_distance`, `_coneAngle`, `_useCameraDirection`, `_socketName`, `_equipSlotName`,
`_forwardAxis`, `_upAxis` and `_detectModeAreaDataType`. The detect volume is a
cone, from the camera or from an equip socket, with a distance. Read those numbers
out of the table and the mod can reproduce the game's own test for which objects
are lit up, instead of guessing a radius.

Two more per-object flags exist, both on `CharacterInfo` rather than on gimmicks,
so they are for characters: `_isVisibleWhenDetectModeOnly` and
`_isMapIconAlwaysShow`. `TribeInfo` has `_detectModeShowEnemy`.

## The map icon create path, disassembled

Read out of `CrimsonDesert.exe` build 2.01.00, exe 1.0.0.2760, on 10 September
2026. ImageBase 0x140000000, relocations stripped, so every RVA below is a fixed
runtime address. Section names in this image are meaningless: `funcbounds.py`
puts executable code in `.data2`.

Start with the correction. There is no `AUCreateMapIconInfo`. `.?AU` is the MSVC
mangling prefix for a struct, so two characters of the encoding were read as part
of the name. The struct is `pa::uiCommonScript::CreateMapIconInfo`.

### The constructor

RVA `0xD2A250`, bounds `0xD2A250` to `0xD2A54D`, 765 bytes, from the `.pdata`
RUNTIME_FUNCTION table. Its destructor sits at `0xD2A550`, immediately after, and
is slot 0 of the struct's vtable at `0x0555A830`. The base class vtable
`IUIAsyncLoadData` is at `0x0555A820` with two slots, and the constructor writes
both pointers to offset 0 in the usual base-then-derived order.

A minimap twin exists at RVA `0xD828E0`, also 765 bytes, with the same shape.

Object size is `0x80`, 128 bytes. This is worth stating plainly because it is easy
to misread:

```
+0x0D2A2A9: mov  ecx, 0x1fd
+0x0D2A2AE: mov  rax, qword ptr gs:[0x58]
+0x0D2A2B7: mov  rsi, qword ptr [rax]
+0x0D2A2BA: mov  edx, 0x10
+0x0D2A2BF: cmp  byte ptr [rcx + rsi], r13b
+0x0D2A2C3: mov  ecx, 0x80
+0x0D2A2C8: je   0x140d2a2d1
+0x0D2A2CA: call 0x1446fc948
+0x0D2A2CF: jmp  0x140d2a2d6
+0x0D2A2D1: call 0x1446fc880
```

`0x1FD` is a TLS byte index, read through `gs:[0x58]`, that picks between two
allocators. The size handed to the allocator is `ecx = 0x80` with `edx = 0x10`
alignment. The same `0x1FD` index comes back at `0xD2A49D` for the free path. The
field span below runs to `+0x78` plus one byte, which fits 128 exactly.

### The arguments

Fourteen of them, four in registers and ten on the stack. The frame uses
`lea rbp, [rsp - 7]`, so stack argument 5 is `[rbp + 0x57]` and each later one is
eight higher.

| arg | where | goes to | evidence |
|---|---|---|---|
| 1 | RCX | not stored, used as `this` | `mov rdi, rcx`, later `call [rdi+0x110]` |
| 2 | RDX | `+0x08` word | `movzx eax, word [r12]` then `mov [rbx+8], ax` |
| 3 | R8 | `+0x10` qword and `+0x18` byte | `mov rax,[r14]` and `movzx eax, byte [r14+8]` |
| 4 | R9 | `+0x20` dword | `mov eax,[r15]` then `mov [rbx+0x20], eax` |
| 5 | `[rbp+0x57]` | `+0x24` and `+0x2C` | `vmovsd [rbx+0x24], xmm0` then `mov [rbx+0x2c], eax` |
| 6 | `[rbp+0x5F]` | never read | scanned the whole body |
| 7 | `[rbp+0x67]` | never read | scanned the whole body |
| 8 | `[rbp+0x6F]` | reused as scratch | out-param for the hash call |
| 9 | `[rbp+0x77]` | `+0x30` | ref-assigned by `call 0x1403A8220` |
| 10 | `[rbp+0x7F]` | `+0x38` | hashed by `call 0x14122FF40`, r8d=1, r9d=0x2FFFF |
| 11 | `[rbp+0x87]` | `+0x4C` byte | direct copy |
| 12 | `[rbp+0x8F]` | `+0x48` float | `vmovss` |
| 13 | `[rbp+0x97]` | `+0x50` | swap or assign by `call 0x14049C7D0` |
| 14 | `[rbp+0x9F]` | `+0x78` byte | direct copy |

`+0x4D` is set to a hardcoded 1. Defaults written before the argument copies are
`0xFFFF` at `+0x08`, `-1` at `+0x10` and `0x15` at `+0x18`, which is what an unset
type and an unset key look like.

Argument 5 is the position, and it is the one the mod cares about:

```
+0x0D2A3A0: mov    rax, qword ptr [rbp + 0x57]
+0x0D2A3A4: vmovsd xmm0, qword ptr [rax]
+0x0D2A3A8: vmovsd qword ptr [rbx + 0x24], xmm0
+0x0D2A3AD: mov    eax, dword ptr [rax + 8]
+0x0D2A3B0: mov    dword ptr [rbx + 0x2c], eax
```

Twelve bytes copied through a pointer, which is a float3. That it is world space
rather than canvas space is inference, not something read from the binary, and it
is the first thing a live breakpoint should settle.

Argument 3 points at a structure of at least nine bytes, a qword and a byte, which
is the shape of a `UIMapIconKey`. Argument 10 is a `const char*` that gets hashed
into a four-byte field, so the icon is named by string, which is where a
`MapIcon_` key such as `MapIcon_DetectEffect` would go.

### Two calls on the controller

Before allocating anything, the constructor makes a virtual call on the object
passed in RCX:

```
+0x0D2A287: vmovups xmm0, xmmword ptr [r8]
+0x0D2A28C: vmovups xmmword ptr [rbp - 0x39], xmm0
+0x0D2A291: mov     rax, qword ptr [rcx]
+0x0D2A299: mov     r9d, dword ptr [r9]
+0x0D2A29C: lea     r8, [rbp - 0x39]
+0x0D2A2A0: movzx   edx, word ptr [rdx]
+0x0D2A2A3: call    qword ptr [rax + 0x560]
```

That is vtable slot 172, taking the dereferenced word, a pointer to a stack copy
of the key, the dereferenced dword, and a zero byte on the stack.

At the tail it calls slot 34, `[rdi+0x110]`, takes `[rax+0xB0]`, builds two
24-byte descriptors on the stack, and calls `0x143C2DF20` with the finished object
in R9. That last call inserts into a keyed container.

### The dispatcher

RVA `0xD2F620`, and it holds the only call to the constructor:

```
+0x0D2F8D3: mov  r8, qword ptr [rbp + 0x57]
+0x0D2F8D7: mov  rdx, rsi
+0x0D2F8DA: mov  rcx, rdi
+0x0D2F8DD: call 0x140d2a250
```

The minimap side pairs `0xD801E0` with `0xD828E0` the same way.

### What is reachable today

Calling the constructor needs a live pointer to a
`UIGamePlayControlCommon_MapIcon`-family controller for RCX, and no static holder
of one was found anywhere in the image. Both the constructor and the dispatcher
dereference RCX and call through its vtable before touching any icon field, so a
fabricated controller crashes on the first instruction that matters. That pointer
has to come from a breakpoint on `0x140D2F620` in a running game, not from static
analysis.

The cheap thing works now. `pa::ClientMinimapActorComponent` has a vtable at RVA
`0x054ACC08` with a matched pair:

```
slot 1 -> 0x8FC500, a jmp thunk to 0x99D4910:
  mov byte ptr [rcx + 0x10], 1
  mov rax, rdx
  mov dword ptr [rdx], 0
  ret

slot 2 -> 0x1385B70:
  mov byte ptr [rcx + 0x10], 0
  ret
```

Enable and disable of a per-actor minimap flag at `+0x10`. Slot 1 also takes a
second argument in RDX, zeroes the dword it points at, and returns it, so it is
not a bare setter and needs a scratch dword.

MasterLooter's existing component scan can reach this. It already walks an
entity's component slots from `kOff_Ent_Comps = 0x68` and matches on the RTTI name
through `RttiName()` in `mem.cpp`, so finding `ClientMinimapActorComponent` on an
actor is a class-name constant away. Call through the vtable slot, never the body
address: slot 1's body sits behind a jmp thunk that can move between builds while
the slot index stays put.

What that buys is a toggle on an icon association the actor already carries. It
does not choose a position or an icon type. The arbitrary-position route stays
blocked on the controller pointer.

## Where the controller pointer lives

Answered 10 September 2026, statically, no debugger needed. Every RVA below came
out of a command that can be run again.

### The owning classes

Both dispatchers sit in vtables, found with `qref.py`, and the vtables resolve
through their MSVC complete object locators:

```
RVA 0x0555D1C0 is slot 170 of vtable 0x0555CC70
   class .?AVUIGamePlayControlRootWorldMap@uiCommonScript@pa@@
RVA 0x0555E4C0 is slot 170 of vtable 0x0555DF70
   class .?AVUIGamePlayControlRootMiniMap@uiCommonScript@pa@@
```

So the RCX the create path wants is the root UI control for the world map or the
minimap, and creating an icon is virtual slot 170 on it, vtable offset `+0x550`.
The `call [rax+0x560]` the constructor makes on the way in is slot 172 of the same
object, and the `call [rdi+0x110]` at its tail is slot 34.

### The pointer arrives on its own

Each map surface has a per frame update, at RVA `0xd205f0` for the world map and
`0xd8aff0` for the minimap. Both resolve into the same two vtables:

```
RVA 0x0555CD88 is slot 35 of vtable 0x0555CC70   UIGamePlayControlRootWorldMap
RVA 0x0555E088 is slot 35 of vtable 0x0555DF70   UIGamePlayControlRootMiniMap
```

The two classes are laid out identically:

| what | slot | vtable offset |
|---|---|---|
| update | 35 | `+0x118` |
| create map icon | 170 | `+0x550` |

RCX at slot 35 is the controller. Hook the update, cache RCX, call slot 170 on the
cached pointer. Nothing else is needed, and there is no global to find.

That hook is already proven on this build, installed and running hot on both
surfaces. The evidence is in `private/CRIMSON-ROUTE.md`, along with the fact that
another mod owns both slots today, so expect the ordering argument the D3D12
present hook already has and plan to stack rather than wrap.

### The registry, for completeness

There is also a static route, and it is worth recording even though the update
hook is cheaper.

The root control is built by a factory at RVA `0xE3CC40`, which allocates `0xC48`
bytes and calls the constructor at `0xD07790`. Its one caller is a control-open
routine at `0xE21210` that begins:

```
+0x0E2122D: lea  r14, [rip + 0x5d70680]      ; key object at RVA 0x06B918B4
+0x0E21234: mov  rdx, r14
+0x0E21237: add  rcx, 0x31228
+0x0E2123E: call 0x1404045f0                 ; lookup(container, key)
+0x0E21243: test rax, rax
+0x0E21246: je   0x140e21258                 ; miss, go create
```

and later retrieves through a second container:

```
+0x0E21337: lea  rcx, [rdi + 0x31208]
+0x0E2133E: mov  rdx, r14
+0x0E21341: call 0x1404045f0
```

So a UI manager holds keyed containers at `+0x311E8`, `+0x31208` and `+0x31228`,
with a counter at `+0x311F4`, and `0x1404045F0` is the lookup. The key for the
world map root is a runtime-initialised static at RVA `0x06B918B4`, which sits in
the zero-filled tail of `.srdata` and so is built by a startup initialiser at a
fixed address a mod could read.

What this route still needs is the manager pointer itself. Its only reachable
holder found so far is `rdi` inside a 30 KB function at `0xE0B270`, which is where
the trail would continue. The update hook makes that unnecessary.

### Tools

Two scripts were written for this work and live in this mod's own folder, at
`docs/investigations/`.

`vtres.py` takes an RVA inside a vtable, walks back to the complete object
locator, and prints the class name, the slot index and the vtable offset. Master
Looter's `vtable.py` misreads its slot index, which is why this exists.

```
py -3 docs/investigations/vtres.py 555D1C0
  RVA 0x0555D1C0 is slot 170 of vtable 0x0555CC70  (vtable offset +0x550)
  class pa::uiCommonScript::UIGamePlayControlRootWorldMap
```

`xref.py` finds RIP-relative references to any RVA across the image, which covers
both the lea that loads an address and the call rel32 that reaches a function,
then keeps only the hits that land inside a real function according to the
`.pdata` table. That is what `qref.py` cannot do, and it found every step of the
chain above, one command per step. It needs numpy, which is a system install on
this machine rather than something vendored.

Three more came from Master Looter and now sit beside them, so this mod does not
reach into that project for anything. `disasm.py` disassembles from an RVA,
`funcbounds.py` gives exact function bounds from the `.pdata` table, and
`qref.py` finds 8-byte pointers to an RVA, which is what turns a function address
into the vtable that holds it.

They are unchanged apart from two things. `disasm.py` no longer adds Master
Looter's vendored dependency folder to the path, since this folder does not
vendor capstone; it uses the installed one, and prefers a local `vendor` folder
if one is ever added. `qref.py` gained the usage docstring it never had. Each carries a
line saying where it came from and when, so a future divergence from Master
Looter's copies is visible rather than silent.

## Confirmed at runtime: the controller pointer, read-only

Session six, 10 September 2026, probe 0.1.0. No hooks, nothing called.

Both root controls were found by scanning resident private memory for their
vtable pointers, and both held for the whole session, six passes and a map open:

```
UIGamePlayControlRootWorldMap   0x0000034E7D82EE00   region 0x34E7A000000 +0x43B0000
   slot 170 create icon         0x0000000140D2F620
   +00 000000014555CC70  +08 0000034F5F0B5500  +10 0000000000000000

UIGamePlayControlRootMiniMap    0x0000034F5F567E00   region 0x34F5DC20000 +0x71A0000
   slot 170 create icon         0x0000000140D801E0
   +00 000000014555DF70  +08 0000034F69613C00  +10 0000000000000000
```

Slot 170 on each is the dispatcher static analysis named, to the byte. The
world map root existed in pass one, eighteen seconds after launch and before the
map was opened, so the controls are built at UI startup and kept. That is what
makes a cached pointer safe.

Slot 35 on both reads `0x7FF854...`, outside the game module: Crimson Route's
detour, visible in the vtable from the outside. Route's own logs said it owned
that slot and this is the first independent confirmation.

Exactly one of each. There is no ambiguity about which instance is live.

### What it cost to get here

Six sessions, three of them mine to answer for.

- Session one: the log opened in the CRT's Unicode mode and the first narrow
  write took the game down at startup. Fixed, plus an invalid parameter handler.
- Session two: every one of 112 hits was an entry in a table pairing vtables with
  their RTTI names. A hit whose `+08` is itself a vtable, or a region answering to
  many classes at once, is now discarded.
- Sessions three and four: scans timed out having read a fraction of memory.
  Touching a committed page that is not resident costs a soft fault per page,
  about a fortieth of the throughput; the probe now asks Windows which pages are
  in the working set and reads only those.
- Session five: crash to desktop. Map icons are freed each time the map closes,
  and my reads of a candidate had a pre-check but no exception guard. Every read
  of game memory is inside a handler now, with a last one on the thread entry.
- Session six: both pointers, stable.

The self test, which builds an object of its own and hunts for it before each
session, is what turned "no result" from a mystery into a measurement.

### Next

The next step is the first write-class action: calling slot 170 on the cached
world map controller with a real position. It needs the fourteen arguments from
the section above filled in, and the first attempt should be made with the map
open and the game saved, because a wrong argument is a crash. That call is the
point of the whole exercise and it does not get made without a decision.

Two smaller things for the next build. Regions over 256 MB are still skipped and
pass four skipped one of 275 MB; with residency filtering the size cap costs
little and should rise. And the slot counter walks past the end of short vtables,
so the slot count it logs is only meaningful for the two root controls.

## Confirmed at runtime: a native pin, placed by the mod

Session ten, 10 September 2026, probe 0.3.1. The first call the plugin ever made
into the game placed a custom marker on the world map, and it showed up.

```
[spy world #430] game pin   type=0x0001 key=0/0x15    pos=(-9699.016, 0.000, -4551.179) label="Marker"
[replay #1]      our pin    type=0x0001 key=1001/0x15 pos=(-9669.016, 0.000, -4521.179) label="Marker"
[replay #1]      returned 0x1
```

The call is slot 170 on the world map root control, made on the game's own UI
thread from inside a detour on that slot, right after the game placed a pin of
its own. Every argument except two was byte for byte what the game had just
passed: the position moved 30 units, and the key id started at 1001 so it could
never land on the game's own counter. The struct at argument ten went in zeroed,
because the captured one carries a pointer the constructor takes ownership of.

### The argument list, settled

Eleven arguments to slot 170, read from ten sessions of spying and one working
call. The first spy build read thirteen: the dispatcher pushes seven registers
where the constructor pushes five, and reusing the constructor's offsets put
every stack argument two slots off. Return addresses turning up as a "struct"
is what caught it.

| # | where | what | pin value |
|---|---|---|---|
| 1 | RCX | the root control | scanned pointer |
| 2 | RDX | `const uint16_t*` icon type | `0x0001` |
| 3 | R8 | `const {int64 id; uint8 kind}*` key | `{n, 0x15}` |
| 4 | R9 | `const uint32_t*` | `0` |
| 5 | stack | `const float*` | `0` |
| 6 | stack | `const float3*` position, Y is zero for a pin | world X, 0, world Z |
| 7 | stack | `const char*` label, may be null | `"Marker"` |
| 8 | stack | `const char*` icon name, must be non-empty | `"MapIcon_Pin_Marker"` |
| 9 | stack | `uint8_t` by value | `0` |
| 10 | stack | `const struct*` count at +4, pointer at +0x10 | zeroed |
| 11 | stack | `uint8_t` by value | `1` |

Other icon kinds seen through the same slot, for later: `MapIcon_ActorFocus` is
the player, type 0, keyed by actor `A0100001` with kind `0x0C`, and it is created
once at first map open and never refreshed, so it is not a live position.
`MapIcon_StageFog` is type `0x020F` kind `0x02` with the radius in argument five.
`MapIcon_PathFinderDestination` is type `0x0003` with kind `0x15`, the game's own
Set Destination pin, and `0x15` is the constructor's default key kind.

### What the game does with icons

It builds them all once, on the first map open of the session, about four
hundred calls in five seconds, and after that only a pin placement creates one.
Reopening the map creates nothing. That is why session nine's replay, queued to
run behind the next icon, never ran, and why the trigger became the pin
placement itself.

### What is left for the feature

Three things, and none of them is a mystery any more.

The trigger. The replay runs on the game's thread because it piggybacks on a
game call into slot 170, and those only happen at map open and pin placement.
The real feature needs a game-thread moment of its own choosing: hold the flash,
see a glint, drop a pin. The per frame update at slot 35 is that moment. Another
mod already holds that slot on both roots, so the plugin stacks: it reads
whatever the slot holds, forwards to it, and never assumes it is the game's
own function. That is the same shape as Master Looter's present hook and it
needs nobody's agreement, only a build and a session.

The glint. Which objects are lit is data the game holds, `IsStageDetectModeTarget`
and the `fx_detectmode_knowledge_gimmick` effect, and the entity scan from
Master Looter already names every gimmick in range. Joining the two is the part
that is not written yet.

The pin's identity. Key ids from 1001 up are ours; the label is a free string.
Whether the game persists these pins across a save, and whether it lets the
player delete one placed with an id it did not issue, are two things a session
will answer and static analysis will not.

## Confirmed at runtime: the player's position and the flash flag

Session thirteen, 10 September 2026, probe 0.4.2. Both read on the game's
thread, no hooks beyond the two already in place.

The way in is the player's `ClientSpecialModeActorComponent`, which the scan
finds by RTTI. Its `+0x08` is the played body:

```
component +0x08  -> ClientChildOnlyInGameActor           the played body
actor     +0x68  -> component block
block     +0x1A0 -> ClientTransformSyncActorComponent    the transform
transform +0xB4  float3 position, parent-relative
transform +0xC8  u32 parent id, 0xFFFFFFFF for none
transform +0xEC  float3 parent world position
```

Thirty samples over a minute of play moved as the player walked, and the
elevation matched the map's own player marker to the centimetre. The offsets
are Master Looter's; the walk from the special component is new.

The component block, for the record, by slot: user login `+0x08`, child
container `+0x18`, status `+0x20`, sub level `+0x28`, equip slot `+0x38`,
character control `+0x40`, vehicle `+0x48`, **detect `+0x50`**, ai `+0x58`,
effect `+0x60`, frame event `+0x68`, catch `+0x70`, remote catch `+0x78`.
`ClientDetectActorComponent` at `+0x50` is the blinding flash's own per-actor
state, and the next build reads the aim from it.

The flash flag: the special component's `+0x40` holds the player actor id,
`A0100001`, while the flash is on and zero when it is off. `+0xE0` holds an
`IRefCounted` object at the same time and is cleared with it.

### The camera, and why the player stops turning in circles

Three camera sessions found no float moving in any camera object within its
first 1 KB: `CameraManager`, two `PlayerCameraComponent`s, `PlayerCameraTPSMode`.
The view state is elsewhere or further in. It is no longer hunted by the
player. The aim comes from the detect component, which needs no camera at all,
and the view direction for marking things that do not glint will be read out of
the binary from `PlayerCameraComponent`'s methods.

### The camera, found offline

`PlayerCameraTPSMode` (vtable `+0x055F0908`, 109 slots) has its update at slot
19, RVA `0x0113D8B0`, 2578 bytes, 47 multiply, square root and fused ops. It
keeps the camera as scalars on its own object rather than a matrix: five stores
at `this+0xC0`, `+0xC8`, `+0xCC`, `+0xD0` and `+0x104`, each after a clamp or
interpolate helper at `0x1447A5EA4`, and one at `+0x360`. Yaw, pitch and
distance live there. Two stores go through another object at `+0x1DC` and
`+0x1E0`.

The object is reallocated during play. A scan-found pointer to it went stale in
session thirteen, which is why the probe saw no float move in any camera
object. Holding it means a thunk on slot 19 that captures `this` on every
update, the same pattern the minimap tick uses. That is the route for marking
things that do not glint, and it is parked until the detect component route has
been tried, because that one needs no camera at all.

Nothing in the camera family needs the player to turn in circles again.

## Confirmed at runtime: the aim point, and why the first pin went 9 km away

Session seventeen, probe 0.5.0. Two presses with the flash on, the first aimed
at nothing, the second at a glint, every component in the played body's block
copied at each and diffed on the game's thread.

The pin was placed and returned 1, and it was nowhere near the player, because
everything read off the actor so far was in the sub-level's local space and the
map draws world space. The transform carries both:

```
transform +0x0B4  local   (-733.40, 560.54, -301.67)   what Master Looter reads
transform +0x29C  world   (-9733.40, 560.54, -4301.67)  what the map draws
                  origin  (-9000.00, 0.00, -4000.00)   world minus local
```

Copies of the world position sit at `+0x324`, `+0x3D0` and `+0x51C` of the
transform, on the equip slot, character control, effect and frame event
components, and on the detect component at `+0x548` and `+0x554`. It is the
most repeated number in the block. The local one is at `+0xB4`, `+0x12C` and
`+0x63C` of the transform and on the vehicle component at `+0x35C`.

The aim point: `ClientCharacterControlActorComponent+0x318` holds a float3 in
local space that moved when the flash was aimed at the glint and sat 10.7 units
from the player, `(-728.52, 561.17, -292.18)`, world `(-9728.5, 561.2, -4292.2)`.
That is the single read the feature needs, and it is also what any "mark where
the crosshair points" feature would use for things that do not glint.

The actor id route is still wired and untested: no dword in the diff took an
id shape between the two presses, so the game does not appear to record the
target as an id on these components.

## The spec, as of 10 September 2026, and where each piece stands

As I put it at the time: when the chord is pressed, triangulate the point on the ground
being pointed at and put a marker there. When only the blinding flash is on,
pointing directly at or near a glint places a marker automatically, once, and
never a second one where one already is.

### Chord: the ground point

- The game computes this itself. `ClientCharacterControlActorComponent+0x318`
  held a point 10.7 units ahead in local space while the flash was aimed in
  session seventeen, and read zero at the same kind of press in session
  eighteen. Whether it is populated only while the flash button is held is the
  open question, and one press with the button held answers it.
- If it is, that field is the whole feature. If not, the ground point comes
  from the camera's pitch, found offline on `PlayerCameraTPSMode` at `+0xC0`
  through `+0xD0`, held through a thunk on its slot 19 the same way the minimap
  tick is, intersected with the ground at the player's feet.
- Fallback in the build now: a ray from the player along the facing, the yaw
  quaternion at transform `+0x28C`, against every entity in the actor manager's
  list as small spheres in a beam that widens with distance. First hit wins.
  It hits objects, not terrain.

### Flash only: automatic, once, deduplicated

- Once and deduplicated is in the build: every pin the mod places is
  remembered and nothing is placed within eight units of one.
- "Near the glint" is the ray with a wide spread, which is a cone, plus a
  dwell of a second or two so a pin is not placed every frame.
- Which object in the cone is glinting is the open piece. Candidates: the
  detect component's scalars that moved between an unaimed and an aimed press
  in session seventeen, `+0x580` from 12.6 to 3.0 among them, which looks like
  a distance to the current target; the target's own effect component, since
  `fx_detectmode_knowledge_gimmick` is played on it; or the gimmick's
  knowledge state. The build logs the scalars beside the ray's candidates at
  each press so the first of those can be settled from one session.

### Solved and in the build

Player position in world space, transform `+0x29C`. Sub-level origin, world
minus local. Facing, transform `+0x28C`. Flash active, special component
`+0x40`. The actor manager and its entity list. A native pin at any world
position, on the game's thread. Ready within about twenty seconds of entering
the world, with the player's own special mode component and not another
character's.


## The deep dive, 10 September 2026, evening

Five parallel investigations against the exe, each followed by a second
pass whose job was to knock the first one down. Notes and evidence in
`private/re-*.md`. What survived, and what it changes.

### The camera hook was on the wrong class

`PlayerCameraTPSMode`'s vtable has sixteen slots, not 109. The 109 came from
counting plausible function pointers past the end, straight through the next
class's locator into `PlayerCameraBlackHoleMode`'s vtable at `+0x055F0990`.
"Slot 19" at `+0x055F09A0` is that class's slot 2, and `0x113D8B0` is the
black hole camera's update. It fires when the camera is in that special mode
and at no other time, which is why session 23 captured nothing in a minute of
ordinary play. `vtres.py 55F09A0` says so in one line; the earlier work only
ever ran it on the vtable base.

The real update is slot 2, `0x113C100`, `Update(this, float dt)`. It walks
the two blend weights at `+0x1DC` and `+0x1E0` (the "yaw and pitch" of the
earlier note are fade weights) and calls three helpers: `0x113C230`,
`0x113C500`, `0x113CA10`. The third builds the camera from fields on the
object:

```
+0x30  pivot position, float3, then packed cell indices in the high qword
+0x40  rotation quaternion x, y, z, w
+0x50  distance behind the pivot
```

At `0x113CDF8` it computes the forward vector as `(2(xz + wy), 2(yz - wx),
1 - 2(x^2 + y^2))`, normalises it, and places the camera at pivot minus
forward times distance. That forward is the view ray. Pitch is `asin(fwd.y)`,
yaw is `atan2(fwd.x, fwd.z)`, the same convention as the body's facing. Build
0.8.0 hooks slot 2, refuses to install if the slot does not hold `0x113C100`,
and reads the quaternion at each press.

The earlier field list (`+0xC0` through `+0xD0`, `+0x104`, `+0x360`) is
smoothed preset state, not the pose. `+0xC0`/`+0xC4` are stored as a
multiplied pair at the end of `0x113C230`.

### Which objects glint: a byte on the gimmick component

`GimmickEventHandlerData_EnableDetectMode`'s `Execute` (`0x233E3E0`) reads a
byte from its own `+0x58` and calls slot 124 of the actor's
`ClientGimmickActorComponent` (`0x8862F0`), which stores it at
`component+0x45B`. Slot 7 (`0x88DD20`) reads it back. The component sits at
slot `+0x30` of the entity's component block, vtable `+0x054A5A10`. The mesh
highlight is a separate handler writing a different byte through a different
sub-object, so `+0x45B` is the knowledge glint specifically. A second reading
reproduced every instruction.

The effect names (`fx_detectmode_knowledge_gimmick` and the rest) are not in
the exe as text; they live in `effectinfo.staticinfobody` and are resolved by
hash. The success-versus-fail split was not settled; it does not matter for
marking.

Build 0.8.0 reads `+0x45B` on every gimmick in the entity set each pass. The
automatic marker now picks among glinting objects only, nearest the view ray
by angle, held for a second.

### The detect component is not the aim

`FindDetectTargetTask` has five virtuals and none of them cast anything; the
component's slot 42 (`0x8FBB30`) throttles and submits an asynchronous job
through a registry at `0x1F59140`. `task+0x540` held the same actor at every
press across 34 seconds in session 19, so it is not the aimed object.
`detect+0x410` is a clean has-target byte (`0xFF`/`0x00`) and `+0x42C` counts
ticks while a target is held, but "target" there is whatever the job picks,
not the crosshair. `+0x3EC` read `-1`, `FLT_MAX`, `1.5` and `0.5` in different
sessions and pinned the player's own feet when used. All of it stays in the
log and none of it drives a pin.

### The beam was too strict, and 300 entities were fine

Session 21's real hits sat six to eleven units below the player at twenty
units out. The 3D beam of 0.7.0 around a level ray rejected every one of them,
which is why session 23 hit nothing with 300 entities in the set. With the
camera's forward in hand the beam is a true cone around the actual view
direction; without it the level ray keeps an XZ radius and a vertical band of
`4 + 0.35 * along`. Near misses are logged at each press so the next tuning is
against a real case.

The entity set reads world position from `transform+0x29C` for every entity,
while the player walk composes local plus parent. They agree for the objects
the ray has hit so far; parented items may differ. Noted, not fixed.

### Other mods

Nothing public touches the camera pose by hook. Two camera mods (UCM,
CDCamera) edit `playercamerapreset.xml` inside a PAZ archive and disclaim any
memory work. `cd-tomtom-arrow` (Rust, GPLv3) captured player position and a
camera heading write by byte pattern on build 1.08.00; none of its camera or
entity patterns survive in 1.0.0.2760. Trinity's shipped menu targets this
build and claims a live map marker read from a "navigation component", but its
published source lags the binary and has no such code. CrimsonDesertCoop
publishes offset tables that do not match ours and names `pa::ClientActorManager`
from a cheat table. Nothing there is reusable as is, and none of it is needed
now that the pose comes from the game's own camera object.


## The ray cast, and the edge of the world it can see, 10 September 2026, late

### The camera, settled

Three presses at known pitches in session 26 read -89, -0.6 and +19
degrees from the quaternion at `+0x40` of the TPS mode object. It is the
view. The pair at `+0xC8`/`+0xCC` is the over-the-shoulder offset (0.8 right,
0.6 up), and `+0x14C` is that offset turned into world space by the same
rotation, which is how the two were told apart. The update runs once per
frame on one object.

### The game's own ray cast

Two more passes against the exe, each checked against the other, gave:

- `hknpWorld::castRay` at `0x41BDA30`, slot 61 of the world's vtable at
  `0x51FB528`, taking (world, query, collector). Havok here is double
  precision; the collector the game uses is `hknpClosestHitCollector`
  (vtable `0x51FE910`, 0xC0 bytes, hit flag at `+0x0C`, best fraction as a
  double at `+0x10`, the 0xA0 byte result at `+0x20` with its fraction at
  `+0x30`).
- A scan for `call [reg+0x1E8]` sites found the game's wrapper at
  `0x3926550`:

  ```
  bool Cast(void* unused, int layer, bool flag, const float3* start,
            const float3* dir, float maxDist, float* outDist,
            float3* outNormal, bool* outFlag)
  ```

  It subtracts the frame offset at `0x6C16B80`, converts to doubles,
  builds the query and the collector on its own stack, and calls castRay
  through the facade object at `0x6915FB8`. The mod calls the wrapper and
  nothing deeper. The layer word is `(layer < 0x3B ? layer : 0x40000000 |
  (layer - 0x3B)) | flag << 9` beside a `0xFFFF` filter. Layer 0 hits the
  world.
- The frame offset is `{chunkX * 1000, 0, chunkZ * 1000, 0}`, rewritten at
  one site in `0x391EEA0` when the player's 1000 unit chunk changes. It read
  (-9000, 0, -4000) in the test area, so "local" positions in this file are
  chunk positions.

### The window

Sessions 30 and 31: every direct hit sat on a face of a box 160 units a
side around the player, edges on multiples of 32, normals along an axis.
Physics keeps terrain collision only for a 5 by 5 window of 32 unit
heightfield regions (`TerrainHeightFieldCollision_%d_%d`, built by the job
at `0x3416A70`; `hknpDefaultHeightFieldGeometry` is 0x70 bytes with a
self-relative pointer to a row-major uint16 sample array at `+0x20`, dims
at `+0x5C`/`+0x60`, scale and base at `+0x64`/`+0x68`). A ray that leaves
the window reports the edge as a hit.

Build 0.9.3 therefore probes straight down along the view every few units
and takes the first point where the ground rises to meet the ray. Inside
the window that is exact. Beyond it the slope of the last two probes is
carried forward; a guess, labelled as one in the log.

### Beyond the window there is no height on the CPU

Two further passes looked for a wider source and found none:

- Rendering terrain is DDS height tiles streamed by virtual texturing. The
  physics window is a 128 texel readback of them (four readback textures
  on a 0x260 byte object at owner `+0xF48`, built by the ctor at
  `0x3414990`).
- `AttachTerrainQueryFilter` (used to snap things to the ground) queries the
  same physics world, ring by ring.
- `PreCalculateTerrainHeightMinMax` is a load-time step dispatched through a
  runtime registry; no static table.
- `TerrainRegionNaviInfoManager` (singleton pointer at `0x6C60F80`) streams
  per-region records on demand and evicts them after 30 seconds; whether a
  record carries a height was not settled.
- The AI height operators replay baked waypoint heights.

So a press within about 80 units lands where the crosshair meets the
ground. Farther out, the only exact positions the mod has are entities,
which the automatic marker uses at any range. The one route left to an
exact far ground point is the depth buffer: a Direct3D 12 hook that reads
the depth under the centre pixel at present time. It is a larger piece of
work and it is not started.


## The 2.02.00 patch, 11 September 2026

The game updated itself to Steam 2.02.00, exe 1.0.0.2850, at 05:32. Every
recorded address moved and the mod's checks refused every hook, which is
their job. `docs/investigations/rebase.py` re-derives the block in
`signatures.h` from a new exe, and 0.10.0 stops depending on most of it:
vtables fall back to RTTI, the camera update is matched by its first bytes,
the ray cast wrapper by its prologue pattern with the facade and frame
offset decoded from its own operands.

Four audits, each checked by a second pass, went over everything:

- Nothing the mod reads or calls changed in shape. Entity, transform,
  actor manager pools, the gimmick byte and its setter, the detect gate,
  the map icon dispatcher and constructor, the camera update chain (shifted
  by a flat 0x11B0), the ray cast wrapper (byte for byte). Only addresses
  moved, and `signatures.h` already carries the new ones. True slot counts,
  now boundary-checked: map roots 185 each, TPS camera 16, detect component
  47, special mode 44, actor manager 10, transform 71.
- One trap for a future rebase: `hknpWorld`'s primary vtable no longer holds
  castRay at slot 61; it sits on a secondary vtable (locator offset 0x20).
  The mod never reads that vtable, only the facade's, so nothing breaks.
- The patch notes contain nothing about the map, pins, the flash, the
  camera or terrain. No other mod had reacted at research time.
- Runtime-only facts (the flash flag at special `+0x40`, the detect
  scalars, the transform's local quaternion) were never tied to code and
  stay unverified until a session on 2850 exercises them.

Three things the audit found that were never in the notes:

1. **A native pin subsystem.** `PinMarkerIconSaveData`,
   `TrocTrCreateOrChangePinMarkerReq/Ack`, `TrocTrRemovePinMarkerReq/Ack`,
   a `CreateOrUpdatePinMarkerIcon` string, and error codes for too many
   pins and for removing an unknown one. The mod's pins skip all of it and
   go straight to the icon dispatcher with a made-up id, so they are almost
   certainly session-local and not the player's to delete. Routing through
   the request path would make them real.
2. **A second glint candidate on gimmicks.** A sub-object at gimmick
   component `+0x438` holding a custom render value table, reached by
   `GimmickEventHandlerData_DetectLighting` (gimmick vtable slot 167) and
   `SetDetectCustomRenderValueName`. The `+0x45B` byte was set at spawn and
   cleared; this table is where a "lit by the flash" state would live for
   gimmicks. Characters need their own answer; `ClientKnowledgeActorComponent`
   is the open question there.
3. **A screen-space ray cast with a GPU depth readback already exists in
   the engine**, used by two real functions (`0x302C600`, `0x3A12A20`), the
   native analog of the depth-buffer plan for far ground points. What owns
   it and whether it runs every frame is not yet known.
