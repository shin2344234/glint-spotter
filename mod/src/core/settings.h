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
//   MiniPin=0     ; also copy each pin onto the minimap
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
        uint32_t key = 0x91;   // VK_SCROLL
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
        // number refusing the feature: Seth aimed at a glint, the table
        // found exactly one placement on the line, record 13 element 100
        // "Challenge_Sealed_Artifact_Her" at five hundred and ninety-eight
        // metres, and the gate threw it away eighty times in a row. The
        // camera yaw that session, 137.3 degrees, is the bearing to that
        // placement to within a fifth of a degree, so the pick was right and
        // only the ceiling was wrong.
        //
        // Twelve hundred was the replacement and it lasted one build. The
        // ceiling is gone: zero means the search runs as far as the table
        // does, and the table is the whole map. Nothing about a placement
        // nine kilometres away makes it a worse answer than one at nine
        // hundred, because the test that picks it is distance from the line
        // and the nearest one on the line still wins. A number here only
        // ever waits to refuse something real.
        //
        // It stays as a key for anyone who wants their pins kept local.
        float reach = 0.0f;
        // RB plus LB plus A, firing the moment all three are down.
        //
        // Build 0.35.0 moved this to both stick clicks with a third of a
        // second's hold, on the reasoning that three buttons the game uses
        // could fire a mark during a fight. Seth asked for it back the same
        // evening, so back it goes: this is the chord his hands know.
        //
        // Both halves are still keys. Hold is what the sticks bought and it
        // costs nothing to leave at zero; set it to 300 and the chord has to
        // be deliberate.
        //
        // XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100, RIGHT_SHOULDER 0x0200, A 0x1000.
        uint16_t chord = 0x1300;
        uint32_t holdMs = 0;

        // A copy of each pin on the minimap, off by default.
        //
        // The idea was immediate feedback: a mark you can see without opening
        // the map. It went in untested and the first session with it is the
        // session Seth says the marking stopped working, which is enough to
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
        // Seth's rule, and it took fifty-seven sessions to be able to keep
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
        // Session seventy-two named the glint Seth has been chasing since
        // session forty-seven: "Challenge_Sealed_Artifact_Her". The two pins
        // that landed on the wrong things that session were called
        // "sector_-39_-17_sub_1_2" and "sector_-39_-17_sub_2_12", which are
        // not objects at all but the level chunks the world is cut into.
        // The record families worth a pin, from the tally of all 171 in
        // session seventy-four. The ones that name a kind of thing:
        //
        //   Vein_Minerals_South_Silver_Levelindex_62   ore veins, 21 records
        //   Challenge_Sealed_Artifact_Her              Seth's glint
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
