#pragma once
#include <cstdint>

// GlintSpotter.ini, next to the plugin. Sectioned, one section, a few keys.
//
//   [GlintSpotter]
//   Key=91        ; virtual-key code in hex for the trigger. 91 is Scroll Lock.
//   Spy=1         ; log every map icon the game creates, through slot 170
//   Mark=clue,artifact,...   ; node names worth a pin, matched anywhere in
//                            ; the prefab path
//   Kinds=        ; comma list of name fragments a level gimmick must match
//                 ; to earn a pin. Empty, the default, accepts anything that
//                 ; is not a bare level chunk.
//   Survey=0      ; one press per place pins every candidate the old actor
//                 ; search found, labelled by distance. A diagnostic from
//                 ; before the level gimmick table; off by default now.
//   Guess=0       ; pin the node nearest the crosshair when the game has not
//                 ; marked anything. Off: only what the game marks is pinned.
//   Chord=RB+LB+A ; controller buttons that place a mark, held together.
//                 ; Names: A B X Y LB RB LS RS UP DOWN LEFT RIGHT BACK START
//   Hold=0        ; milliseconds the chord must be held before it fires
//   MiniPin=0     ; also copy each pin onto the minimap. Crashed the game in
//                 ; session eighty-two; leave it alone.
//   Rumble=1      ; buzz the pad when a pin lands
//   DirectPad=1   ; read a DualSense or DualShock 4 directly, so the chord works
//                 ; without Steam Input. 0 leaves those pads to Steam Input
//   Rod=8.0       ; metres either side of the sight line a press will look,
//                 ; the same at every distance
//   RayFallback=0 ; let a press that the table cannot answer guess from terrain
//   Sweep=0       ; hunt for live objects by scanning the heap. Slow and no longer
//                 ; needed; it is what stalls the game a few seconds after launch
//   Scan=0        ; the same for the player's own component. Off: the actor manager
//                 ; hands it over for nothing, a little later
//   Verbose=0     ; the investigation output: the record dump, the catalogue, the
//                 ; entity listing, the field probe. Off unless something is wrong
//   PressReach=0  ; how far out a press will look, in metres. Zero means no limit
//   AutoCone=2.0  ; the same for the automatic glint marker, which stays wide
//   Reach=0       ; optional ceiling in metres on how far out a level gimmick
//                 ; may be and still count. Zero, the default, means none.
//   Radius=0      ; optional cap in metres on how far a node can be. Zero,
//                 ; the default, means everything the game has loaded, which
//                 ; is already a few hundred metres and no more.
//
// Master Looter's notes record a whole test round lost to a key appended at the
// end of a sectioned ini, where it landed under the wrong section and read as
// nothing. This reader has one section, so a key anywhere in the file is read,
// and an unknown key is logged rather than dropped.

namespace gs::Settings
{
    struct Values
    {
        // F9. Scroll Lock was the default for seventy builds and it assumes a
        // keyboard that has one, which plenty do not. F9 is free in this game,
        // F12 belongs to Steam's screenshot, F11 toggles fullscreen on a lot
        // of setups, and F1 to F8 are where games put quick slots.
        uint32_t key = 0x78;   // VK_F9
        bool spy = true;
        // What is worth a pin, by name. The names come from the node's own
        // prefab path, the same string Master Looter prints, so
        // "gimmick_item_graymane_clue_01" is matched by "clue". The
        // default is the things a player hunts for; firewood and lamps are
        // deliberately not in it.
        char mark[512] = "clue,artifact,treasure,relic,chest,challenge,standstone,"
                         "socket_collection,gather,ore,herb,flower,mushroom,useartifact,"
                         "puzzle_attach,dial,crank,lever";
        // Zero means no cap. The entity set only holds what the actor
        // manager has handed over, which is the world around the player and
        // nothing beyond it, so a radius is a second limit doing the first
        // one's job. It stays for anyone who wants fewer pins.
        float radius = 0.0f;
        // How far a placement may be and still be what the crosshair is on.
        //
        // This was a flat five hundred written in when the test glint was a
        // hundred and nineteen metres away. Session seventy-five is that
        // number refusing the feature: I aimed at a glint, the table
        // found exactly one placement on the line, record 13 element 100
        // "Challenge_Sealed_Artifact_Her" at five hundred and ninety-eight
        // metres, and the gate threw it away eighty times in a row. The
        // camera yaw that session, 137.3 degrees, is the bearing to that
        // placement to within a fifth of a degree, so the pick was right and
        // only the ceiling was wrong.
        //
        // Fifteen hundred metres, and zero still means none.
        //
        // I wanted the ceiling gone and it went, and the session after
        // that put a pin three thousand two hundred and ninety-four metres
        // away and called it a glint. Over seventeen thousand placements, a
        // crosshair pointed at open ground will always find something on its
        // bearing eventually, and being well aligned is not the same as being
        // visible. No angle can tell those apart; only a distance can.
        //
        // Fifteen hundred is two and a half times the six hundred I wanted to
        // be able to reach, so it refuses nothing I have ever aimed at. Set
        // Reach=0 for no ceiling at all.
        float reach = 1500.0f;
        // RB plus LB plus A, firing the moment all three are down.
        //
        // Build 0.35.0 moved this to both stick clicks with a third of a
        // second's hold, on the reasoning that three buttons the game uses
        // could fire a mark during a fight. I wanted it back the same
        // evening, so back it goes: this is the chord my hands know.
        //
        // Both halves are still keys. Hold is what the sticks bought and it
        // costs nothing to leave at zero; set it to 300 and the chord has to
        // be deliberate.
        //
        // XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100, RIGHT_SHOULDER 0x0200, A 0x1000.
        uint16_t chord = 0x1300;
        uint32_t holdMs = 0;

        // How far either side of the sight line a press looks, in metres,
        // the same at every distance. A rod, not a cone.
        //
        // The reasoning, and it is better than the four cones that came
        // before it: a press fires the instant you ask, so it does not need
        // the slack an automatic marker needs. An angle is slack that grows,
        // and growing slack is the wrong shape here. A fifth of a degree is
        // thirty-five centimetres at a hundred metres and three and a half at
        // a kilometre, so every version of this has been most generous exactly
        // where the table is densest and a wrong answer easiest to find. Every
        // wrong pin this feature has produced landed further away than the
        // thing I was pointing at, and that is why.
        //
        // A metre the whole way inverts it. Up close it is a wide angle and
        // forgiving of a building whose origin is not where you aimed on it.
        // At a kilometre it is three hundredths of a degree, so a distant
        // thing has to be genuinely under the crosshair to win.
        //
        // The automatic search keeps its cone. That one fires on its own
        // during a flash with nobody aiming deliberately, often from the air
        // where the crosshair sways, and a miss there is a glint that never
        // got marked.
        // How far out a press will look. Eight hundred metres.
        //
        // Session eighty-seven printed the sight line band by band and the
        // shape of it is the whole story. The closest placement to the line
        // was ninety-nine metres off at fifty, twenty-six off at a hundred and
        // sixteen, seven off at three hundred and seventy-five, and six tenths
        // of a metre off at seven hundred and twenty-five.
        //
        // That is not aim getting better with range, it is arithmetic. The far
        // bands are wider and hold four times as many placements, so something
        // is always nearly on the line out there. Every aperture this feature
        // has tried, cone or rod, therefore preferred distant things, and
        // nearest wins could not save it because the near bands hold nothing
        // within tens of metres of the line.
        //
        // Eight hundred was that limit and it lasted one session, because
        // the height test turns out to do the same job better. The first log
        // with heights in it, on one bearing: the placement fifteen hundred
        // metres out sat six metres off the sight line's height and two tenths
        // of a metre off the line itself, while its neighbours at seventeen
        // hundred sat a hundred and sixty-six metres below. Height separates
        // those; a range cap only refuses all three.
        //
        // So zero, no limit, which is what I wanted in the first place.
        // A number here refuses something real, and the thing that stops the
        // statistics is a candidate having to be at the right height as well
        // as the right bearing.
        float pressReach = 0.0f;

        // Eight metres, and this one is arithmetic rather than another guess.
        //
        // The things I was missing sat between three hundred and
        // five hundred metres out, and past five hundred. Thirty-five
        // centimetres at four hundred metres is five hundredths of a degree.
        // Nobody aims that well, on a controller, in the air, and the once it
        // hit exactly was luck. A rod that narrow can only work at the ranges
        // where it is a wide angle, which is the twenty-five metres a press
        // refuses to look at anyway.
        //
        // So the width has to come from how steadily a hand holds a crosshair,
        // and that is somewhere around a degree. At four hundred metres a
        // degree is seven metres, at five hundred nine, so eight covers the
        // range I work at. The band log said the same thing from the other
        // side: the closest placement to my line at three hundred and
        // seventy-five metres was seven and a bit off it.
        //
        // The rod shape is still the point and it still does the work. Five
        // metres at sixteen hundred is eighteen hundredths of a degree, so the
        // far placements that kept stealing presses have to be genuinely under
        // the crosshair, while the nearest one on the line still wins.
        float rodMetres = 8.0f;

        // The investigation output, off.
        //
        // Sixty-six sessions of this mod were an investigation, and everything
        // that made it one is still running: a per record dump of the level
        // gimmick table as it loads, a four hundred line string catalogue, a
        // hundred and twenty entity listings per press, and a field probe that
        // writes dozens of lines every three seconds on the thread drawing the
        // frame. That is the stutter I get when the mod comes alive after
        // a save loads, and it is a thousand log lines a minute for a feature
        // that works.
        //
        // What stays on is what explains a decision: which placement was taken
        // and why, the sight line band by band on a press, and every pin.
        bool verbose = false;

        // Whether to walk the heap looking for the player's own component.
        //
        // Off, and this is the freeze I chased for six builds. It is
        // not the logging, not the number of passes, not the timing, and not
        // bandwidth: a millisecond of sleep every four megabytes made it last
        // longer at the same intensity. Reading five gigabytes of another
        // process's live heap is simply not something that can be made
        // polite.
        //
        // On, and I was wrong to turn it off. The claim was that the actor
        // manager hands the same component over for nothing, only later. The
        // manager does find the player and the mod uses it for the entity set,
        // but it has never once produced this component: no session in the log
        // has ever printed the line it would print, and switching the walk off
        // meant the flash marked nothing at all.
        //
        // So the freeze stays until something better is found, because a mod
        // that freezes once and then works beats a mod that never does. What
        // is new is that the walk now looks in the regions holding objects we
        // already have before it looks at everything, which may end it in
        // milliseconds.
        bool scan = true;

        // Whether to hunt for live objects by walking the heap.
        //
        // Off. It reads five gigabytes and takes eighteen seconds, twice, and
        // that is the freeze I get shortly after the mod starts working.
        // It was how the mod found instances of the detect mode classes, and
        // that route was retired in 0.29.0 when the level gimmick table
        // replaced it. Everything the mod actually uses arrives another way:
        // the map roots from the spy and from RTTI, the player from the actor
        // manager, the camera from its vtable, the table from a fixed global.
        bool sweep = false;

        // Whether a press that the table cannot answer may guess from terrain.
        //
        // Off. The collision fallback reaches eighty metres or so and then
        // extrapolates the last slope it measured, and session eighty-five has
        // it placing a marker two hundred and sixty-eight metres out on that
        // extrapolation. A guess dressed as a pin is exactly the complaint,
        // and a press that cannot be answered should say so.
        bool rayFallback = false;

        // And the automatic one, which stays wide deliberately.
        //
        // The two cones want opposite things and that is why they are two
        // numbers. A press is deliberate: you already have the thing on your
        // crosshair. The automatic marker fires during a flash with nobody
        // aiming carefully, often from the air where the crosshair sways, and
        // a cone that misses there is a glint that never got marked at all.
        // Two degrees is fourteen metres at four hundred, and the eight metre
        // floor below it keeps close range forgiving too.
        float autoConeDeg = 2.0f;

        // Feedback, because a marker on a map you are not looking at is not
        // feedback. Still no notification of any kind that a marker was
        // placed or a glint was marked.
        //
        // The game's own text popups have not been found yet. Slot 144 of the
        // alert root names "Toast" in its own code but never fired once in
        // seven minutes of play, so it is not the way in, and the enum the
        // kinds come from says Toast is 6 of 40 without saying who consumes
        // it. That hunt continues.
        //
        // These two do not need it. A short buzz on the pad is unambiguous,
        // arrives the instant the pin lands, and costs one XInput call. The
        // minimap copy is the other half: the game creates every one of its
        // own pin markers twice, once on each surface with the same key, which
        // the spy captured sixty times over. Doing what vanilla does puts the
        // marker on the screen you are already looking at.
        bool rumble = true;

        // Whether a DualSense or DualShock 4 is read straight off HID. On by
        // default, because the alternative is Steam Input, and Steam Input
        // makes the game draw Xbox glyphs on a PlayStation pad. The switch is
        // here for the case nobody could test before it shipped: a pad or a
        // driver that objects to a second reader, or a buzz that fights the
        // game's own output to the pad.
        bool directPad = true;

        // Whether a pin is a marker the map can remove, or only a picture.
        //
        // On. A mark writes a record into the marker list the map's own user
        // interface reads, under an id far above anything the game hands out,
        // and draws the icon on that id. Put the cursor on the pin and the map
        // offers Remove Marker, exactly as it does for one you placed. Press
        // it and the game sends a removal request for that id, which the mod
        // is watching for, and the record and the icon both go.
        //
        // Sessions 103 to 112 are all in that sentence. A marker is two
        // records, one on each end of the game's in-process wire, and only the
        // client end is needed: it is what makes the button offer, and the
        // request it sends is something the mod can answer itself. Nothing is
        // written into the authoritative list the save is built from, so pins
        // do not survive a reload and cannot be left behind.
        //
        // Off draws pins the old way, which nothing can remove.
        bool realMarkers = true;

        // Whether the pins outlive the game.
        //
        // On. Loading a save clears every marker the mod placed, because none
        // of them are in the save, so the mod writes its own list to
        // GlintSpotter.pins beside the plugin and puts them back when a world
        // appears. Removing a pin on the map takes it out of the file too.
        //
        // One file for the whole game, not one per save: no save identity has
        // been found that the mod can read, so a second character sees the
        // first one's pins. Tying them to a save is the next thing on this
        // feature. Deleting the file clears them all. Off keeps everything in
        // memory, where a reload loses it.
        bool keepPins = true;

        // Which of the map's marker pictures a pin uses.
        //
        // The game's create carries two bytes and the map draws from them. A
        // marker placed the ordinary way is 4 and 14; the one I picked with
        // Change Marker came through as 1 and 4, and that is the default here
        // because it is the one I picked.
        //
        // The create refuses anything from 14 up in the first and 15 up in the
        // second, so those are the ranges. What each value looks like is not
        // written down anywhere the mod can read, so the way to find another
        // is to place one by hand and read the pair out of the log.
        uint8_t pinStyle1 = 1;
        uint8_t pinStyle2 = 4;

        // A copy of each pin on the minimap. Off, and this time for a reason
        // that is not a theory.
        //
        // Session eighty-two turned it on and the game died. The log ends one
        // line after the copy:
        //
        //   [pin #1] returned 0x0000000000000001
        //   [pin #1] minimap copy key=100001 returned 0x0000000000000001
        //
        // and nothing follows, no tick, no probe, no next pass a quarter of a
        // second later. The call returned, so it did not fault inside itself.
        // What it did was leave something behind that the next frame drew.
        // That fits the surfaces: the world map is only drawn when it is open,
        // the minimap is drawn every frame, so a bad icon on the minimap is a
        // crash on the very next one.
        //
        // The captures do say the game creates each of its own markers on both
        // surfaces. They do not say the mod can, and twice now this has been
        // the thing that broke a working build. It needs its own session with
        // the minimap root re-verified at the moment of the call, not another
        // switch flipped on the way past.
        //
        // The idea was immediate feedback: a mark you can see without opening
        // the map. It went in untested and the first session with it is the
        // session the marking stopped working in, which is enough to
        // put it behind a key. The two surfaces share nothing the mod can see
        // except the key id, and the world map call returns 1 every time while
        // the minimap call returned a live object pointer once and 1 the next,
        // so the two are not doing the same thing. Until that is understood
        // the proven path is the only one on by default.
        bool miniPin = false;
        // One press at each place pins every candidate inside the cone,
        // labelled with its distance, so the glint can be named by reading
        // one number off the map. Set Survey=0 once that is settled.
        bool survey = false;
        // My rule, and it took fifty-seven sessions to be able to keep
        // it: only the glint gets a pin. With this off the mod places
        // nothing unless the game has set a node's detect mode target
        // byte. With it on, the old bearing guess fills the silence, which
        // is how a bottle four metres away came to be marked as a glint a
        // hundred and nineteen metres off.
        bool guess = false;
        // What a level gimmick must be called to earn a pin, as a comma list
        // of fragments matched anywhere in the name, case insensitive. Empty
        // means anything goes except the level chunks, which are rejected
        // whatever this says.
        //
        // Session seventy-two named the glint I had been chasing since
        // session forty-seven: "Challenge_Sealed_Artifact_Her". The two pins
        // that landed on the wrong things that session were called
        // "sector_-39_-17_sub_1_2" and "sector_-39_-17_sub_2_12", which are
        // not objects at all but the level chunks the world is cut into.
        // The record families worth a pin, from the tally of all 171 in
        // session seventy-four. The ones that name a kind of thing:
        //
        //   Vein_Minerals_South_Silver_Levelindex_62   ore veins, 21 records
        //   Challenge_Sealed_Artifact_Her              my test glint
        //   Mission_PororinVillage_Bell_All_Calphade   mission bells
        //   Abyss_marni_visione_gate_0001              visione gates
        //   Titan_Boss_Sotdae_I_Gimmick                boss gimmicks
        //
        // The rest name a place rather than a thing: 01_tom_dungeon_0001,
        // Calphade_SubInner_0001_Phase00, KliffHouse_Hernand_0001_Phase00,
        // FortAnvil_SubInner_0001_Phase00_00. Those hold hundreds of
        // placements each and are where every wrong pin has come from.
        char kinds[512] = "vein_,challenge,mission,artifact,treasure,relic,clue,"
                          "visione,titan,puzzle,standstone,socket";
    };

    // True when `path` contains any of the Mark list's entries.
    bool Marked(const char* path);

    // Reads the ini beside the plugin. Missing file means defaults, and the
    // defaults are written out so the next run has a file to edit.
    const Values& Load(void* selfModule);
    const Values& Get();
    const char* KeyName(uint32_t vk);
}
