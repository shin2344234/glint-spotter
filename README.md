# Glint Spotter

Puts a map marker on the thing you are looking at, in Crimson Desert 2.02.00.

Aim the blinding flash at a glint and the mod pins the object the flash is
lighting, out to the range you can actually see one. Or point at anything at
all and press a button, and it pins that instead. The pin lands on the world
map where the thing stands, so you can walk away and come back to it.

This is a test build. Read the limitations before you install it.

## What it does

- **Marks a glint automatically.** Hold the blinding flash with a glint on your
  crosshair and a pin appears where it is, once the crosshair has held it for a
  second. Distance is not the limit it used to be: a glint six hundred metres
  out marks as readily as one at twenty.
- **Marks anything on request.** Right bumper, left bumper and A together, or
  Scroll Lock, pins whatever the crosshair is on. Ruins, camps, ore veins,
  bridges, dungeon mouths, shops. Anything the game keeps in its own level data,
  which is most things worth walking to.
- **Buzzes the controller when a pin lands**, because the map is not open at that
  moment and there is nothing else to tell you.
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

**The blinding flash** marks glints and only glints. Use it, put the
crosshair on the glint, and hold it there for about a second. The pad buzzes
when the pin lands. It is deliberately generous about aim, because nobody holds
a crosshair still while flying.

**The button** marks whatever you point at. It fires the instant you press, and
it is fussy about aim on purpose: at four hundred metres you have about eight
metres either side of the line. If it misses something, the log says how far off
the line the nearest thing was, and `Rod` in the ini is that number.

Everything is in `GlintSpotter.ini` beside the plugin, and every key has a
comment saying what it does. The two worth knowing are `Rod`, which is how
precisely a press has to be aimed, and `Kinds`, which is what the flash is
willing to mark.

## Limitations

Read these before you decide whether the build is for you.

- **You cannot delete the mod's pins.** Your own markers delete normally; these
  do not. They are drawn onto the map rather than stored as markers, so the
  game's delete has nothing to act on. Loading a save clears them. This is the
  thing being worked on.
- **The game hitches once, for about fifteen seconds, shortly after a save
  loads.** The mod is searching memory for your character. Everything works
  afterwards. Setting `Scan=0` removes the hitch and stops the flash from
  marking anything, so it is not much of a trade yet.
- **A press can mark the wrong thing.** It picks the nearest object in the
  table that your sight line passes near, and if what you are pointing at is not
  in that table, something behind it might be. The log names what it took and
  what else was close.
- **No on-screen message.** The buzz is the only feedback. The game's own popup
  system has not been found yet.

## Reporting a problem

`GlintSpotter.log` appears in `bin64` beside the plugin and is overwritten each
run. Send it with a note about what you were pointing at and how far away it
was. That log is how nearly every fix in this mod was found, and a description
without it usually is not enough.

If a marker lands somewhere wrong, the most useful thing you can say is roughly
how far away the thing you meant was. The log prints the sight line in distance
bands, so that one number is usually enough to place the mistake.

Set `Verbose=1` before a run if something is badly broken. It writes about a
thousand lines a minute and stutters the frame, so turn it back off afterwards.

## Compatibility

Built against Crimson Desert 2.02.00, executable 1.0.0.2850. A game patch moves
the addresses it reads, and the mod checks each one against the running game
before touching it, so a patched game gets a mod that does nothing rather than a
crash.

It runs alongside other ASI plugins, including Crimson Route and Master Looter.
It only ever replaces vtable slots it has verified, and it forwards to whatever
was there before, so two mods on one slot both work.
