#pragma once
#include <cstdint>

// GlintSpotter.ini, next to the plugin. Sectioned, one section, a few keys.
//
//   [GlintSpotter]
//   Key=91        ; virtual-key code in hex for the trigger. 91 is Scroll Lock.
//   Spy=1         ; log every map icon the game creates, through slot 170
//   Mark=clue,artifact,...   ; node names worth a pin, matched anywhere in
//                            ; the prefab path
//   Survey=1      ; one press per place pins every candidate, labelled by
//                 ; distance, so the glint can be named off the map
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
        // One press at each place pins every candidate inside the cone,
        // labelled with its distance, so the glint can be named by reading
        // one number off the map. Set Survey=0 once that is settled.
        bool survey = true;
    };

    // True when `path` contains any of the Mark list's entries.
    bool Marked(const char* path);

    // Reads the ini beside the plugin. Missing file means defaults, and the
    // defaults are written out so the next run has a file to edit.
    const Values& Load(void* selfModule);
    const Values& Get();
    const char* KeyName(uint32_t vk);
}
