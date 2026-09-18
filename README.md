# Glint Spotter

Puts a map marker on the thing you are looking at with Blinding Flash or a
button press.

Aim Blinding Flash at a glint and the mod pins the object it is
lighting, out to the range you can actually see one. Or point at anything at
all and press a button, and it pins that instead. The pin lands on the world
map where the thing stands, so you can walk away and come back to it, and the
map's own Remove Marker takes it off again when you are done.

An ASI plugin for Crimson Desert 2.03.00. Read the limitations before you
install it.

## What it does

- **Marks a glint automatically.** Hold Blinding Flash with a glint on your
  crosshair and a pin appears where it is, once the crosshair has held it for a
  second. Distance is not the limit it used to be: a glint six hundred metres
  out marks as readily as one at twenty.
- **Marks anything on request.** Right bumper, left bumper and A together, or
  F9 on the keyboard, pins whatever the crosshair is on. Ruins, camps, ore veins,
  bridges, dungeon mouths, shops. Anything the game keeps in its own level data,
  which is most things worth walking to.
- **Buzzes the controller when a pin lands**, because the map is not open at that
  moment and there is nothing else to tell you.
- **Removes a pin with the map's own button.** Put the cursor on one and the
  prompt changes to Remove Marker, the same as for a marker you placed by hand.
  Press it and the pin goes.
- **Refuses a glint it can see a hill in front of.** Where the game has collision
  loaded, the mod samples the ground along the sight line and will not pin
  through a rise. Past that range it says nothing rather than guessing.

## Installing

Ultimate ASI Loader (`winmm.dll`) has to be in the game's `bin64` folder. With
the game closed, copy `GlintSpotter.asi` into `bin64` next to it. Copy
`GlintSpotter.ini` there as well, or let the mod write one the first time it
runs.

Start the game, load a save, and wait. The mod needs about forty seconds after
the world appears to find everything it needs, and it hitches the game once
while it does. That is a known problem, see below.

## Using it

Both triggers do the same thing, and it is worth knowing which one you want.

**Blinding Flash** marks glints and only glints. Use it, put the
crosshair on the glint, and hold it there for about a second. The pad buzzes
when the pin lands. It is deliberately generous about aim, because nobody holds
a crosshair still while flying.

**The button** marks whatever you point at. It fires the instant you press, and
it is fussy about aim on purpose: at four hundred metres you have about eight
metres either side of the line. If it misses something, the log says how far off
the line the nearest thing was, and `Rod` in the ini is that number.

**To remove a pin**, open the map, put the cursor on it, and press the button
the prompt offers, which changes to Remove Marker exactly as it does over one of
your own. There is no extra key and no chord, and the pin goes out of the
file at the same time, so it stays gone across a reload.

Both binds are yours to change. `Chord` takes controller button names, so
`Chord=LB+RB+X` or `Chord=RS` work as written. `Key` takes a key name: `F9` by
default because the game binds nothing to it, and `Insert`, `Home`, `Numpad5`,
a bare letter and the rest are all accepted. The log prints both back in words
on startup, so you can see what it read.

A DualSense or DualShock 4 works with Steam Input off, which keeps the game's
PlayStation button pictures. The mod reads the pad directly and the names map by
position: LB is L1, RB is R1, A is Cross, B is Circle, X is Square, Y is
Triangle, Back is Share or Create, Start is Options. So the default chord is L1,
R1 and Cross. It buzzes over USB; over Bluetooth the chord works but there is no
buzz yet. `DirectPad=0` hands the pad back to Steam Input if you would rather.
Switch Pro and other pads still need Steam Input.

Everything else is in `GlintSpotter.ini` beside the plugin, and every key has a
comment saying what it does. The two worth knowing are `Rod`, which is how
precisely a press has to be aimed, and `Kinds`, which is what Blinding Flash
is willing to mark.

## Limitations

Read these before you decide whether the build is for you.

- **The pins live in a file, not in your save.** Loading a save clears every
  marker the mod placed, so the mod keeps its own list in `GlintSpotter.pins`
  beside the plugin and puts them back once you open the map. Nothing it does
  touches your save file, so uninstalling leaves nothing behind. Delete that
  file to clear every pin at once. Pins belong to the save you were in when you
  dropped them: the mod watches which save file the game opens, so a second
  character has its own. Saving into a slot you have never used before takes
  that save's pins along with it.
- **The game can freeze for a second or two shortly after a save loads.** The
  mod is searching memory for your character. It happens once per launch and
  everything works afterwards. Setting `Scan=0` removes it and stops Blinding
  Flash marking anything, so it is not much of a trade.
- **Loading a save from inside the game costs a few seconds.** Your pins, the
  press marker and the map buttons come back within a second or two. Blinding
  Flash takes longer, up to about half a minute, because the piece it needs is
  only findable the slow way.
- **A press can mark the wrong thing.** It picks the nearest object in the
  table that your sight line passes near, and if what you are pointing at is not
  in that table, something behind it might be. The log names what it took and
  what else was close.
- **No on-screen message.** The buzz is the only feedback. The game's own popup
  system has not been found yet.

## Reporting a problem

`GlintSpotter.log` appears in `bin64` beside the plugin, and the last five runs
are kept. Send it with a note about what you were pointing at and how far away
it was. That log is how nearly every fix in this mod was found, and a description
without it usually is not enough.

If a marker lands somewhere wrong, the most useful thing you can say is roughly
how far away the thing you meant was. The log prints the sight line in distance
bands, so that one number is usually enough to place the mistake.

Set `Verbose=1` before a run if something is badly broken. It writes about a
thousand lines a minute and stutters the frame, so turn it back off afterwards.

## Source and licence

MIT licensed. The source, the build script and the notes from working the mod
out are at [github.com/shin2344234/glint-spotter](https://github.com/shin2344234/glint-spotter).

## Discord and Patreon

The Discord is [Shin234's Mods 'n Stuff](https://discord.gg/AZ2ztQYy74), for a
quick question or to see what is being worked on before it ships. Bug reports
still do the most good on the Nexus bugs tab, where they stay attached to the
mod.

Patreon is [patreon.com/cw/Shin234](https://www.patreon.com/cw/Shin234), from $3
a month, with the posts at
[patreon.com/cw/Shin234/posts](https://www.patreon.com/cw/Shin234/posts) because
the new layout hides them. Public mods stay free and no update or fix goes
behind a paid tier.

## Compatibility

Built against Crimson Desert 2.03.00, executable 1.0.0.2944. A game patch moves
the addresses it reads, and the mod checks each one against the running game
before touching it, so a patched game gets a mod that does nothing rather than a
crash.

It runs alongside other ASI plugins, including Crimson Route and Master Looter.
It only ever replaces vtable slots it has verified, and it forwards to whatever
was there before, so two mods on one slot both work.
