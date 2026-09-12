"""Read a Crimson Desert .palevel and pull out what is placed where.

A .palevel is the game's level file: which objects a level contains and where
each one stands. It is the only source found so far for object positions at
arbitrary range, because the actor manager only carries objects once the player
is near them.

Get one out of the archives first, with the unpacker in the Master Looter tree:

    cd "C:\\working\\cd mods\\master looter\\tools\\crimson-desert-unpacker\\python"
    python paz_unpack.py "<game>\\0015\\0.pamt" --paz-dir "<game>\\0015" \\
        -o out --filter "*graymane_camp_lv02_after.palevel"

Archive 0015 holds all 19,771 of them under leveldata/.

FORMAT, worked out against graymane_camp_lv02_after.palevel (283,246 bytes):

The file is self-describing. It opens with magic "PARC" and then a schema
region that declares one block per placed object, in order. A block is

    FF FF  u32  8 zero bytes  u32  u16 value_size
    u32 len + type name                     e.g. "SceneObject"
    u16 field_count                         14 for SceneObject
    per field:
        u32 len + field name                e.g. "_worldTransform"
        u32 len + type name                 e.g. "Transform"
        u16, u16, u32 size                  size is the field's byte width
    u32 len + prefab path                   the object's own prefab

`value_size` is the total width of that object's values, 241 for SceneObject,
and the per-field sizes add up to it. So the offset of any field inside an
object's value block is the running sum of the sizes before it.

A Transform is 40 bytes: quaternion x y z w, then position x y z, then scale
x y z. Every object carries two, `_worldTransform` and `_tiledTransform`, and
they hold the same point in two frames: the tiled one is the world one minus
the level's tile origin, which reads directly as their difference. In
graymane_camp_lv02_after that origin is (-10000, 0, -4000), and the runtime
frame the mod already handles reports (-9000, 0, -4000) for the area I
tests in, so these are the same two frames the mod knows.

WHAT THIS DOES AND DOES NOT DO

--schema and --types are complete and trustworthy: the declared objects, their
prefabs, their fields and the byte offset of every field.

--positions finds Transform pairs by their shape, a unit quaternion followed by
a position and a plausible scale, with a second Transform 0x28 later whose
position differs by a constant. That is reliable for the transforms it finds
but it does not find all of them, and it cannot yet say which object each one
belongs to. Pairing a position with its prefab needs the value region walked
block by block, which is the piece still missing. Do not treat --positions
output as a complete placement list.

Usage:
    py -3 palevel.py <file.palevel> --schema
    py -3 palevel.py <file.palevel> --types
    py -3 palevel.py <file.palevel> --positions
"""
import argparse
import io
import math
import struct
import sys


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def pstring(b, o, limit=400):
    """A u32 length followed by that many printable bytes, or None."""
    if o + 4 > len(b):
        return None
    n = u32(b, o)
    if n < 1 or n > limit or o + 4 + n > len(b):
        return None
    s = b[o + 4:o + 4 + n]
    if any(c < 32 or c >= 127 for c in s):
        return None
    return s.decode("ascii"), o + 4 + n


class Field(object):
    def __init__(self, name, type_name, size, offset):
        self.name = name
        self.type = type_name
        self.size = size
        self.offset = offset

    def __repr__(self):
        return "%s:%s@%d+%d" % (self.name, self.type, self.offset, self.size)


class Block(object):
    def __init__(self, at, type_name, value_size):
        self.at = at
        self.type = type_name
        self.value_size = value_size
        self.fields = []
        self.children = []
        self.prefab = None

    def field(self, name):
        for f in self.fields:
            if f.name == name:
                return f
        return None


MARKER = b"\xFF\xFF"


def read_fields(d, o):
    """u16 count, then count times (name, type, 8-byte trailer). None if it
    does not read as a field list."""
    n = len(d)
    if o + 2 > n:
        return None
    count = u16(d, o)
    # Zero is legal. SceneObjectActorInfoContainer declares no fields at all
    # and is followed straight by the prefab, which is what stopped the walk
    # at eighteen objects of two hundred.
    if count > 200:
        return None
    o += 2
    fields = []
    run = 0
    for _ in range(count):
        gf = pstring(d, o)
        if not gf:
            return None
        fname, o = gf
        gt = pstring(d, o)
        if not gt:
            return None
        tname, o = gt
        if o + 8 > n:
            return None
        size = u16(d, o + 2)
        o += 8
        fields.append(Field(fname, tname, size, run))
        run += size
    return fields, o


def read_body(d, o, type_name, depth=0):
    """One object: its field list, then whatever it nests, then its prefab.

    `_components` and `_childSceneObjects` are declared as ReflectObjectPtr
    with a size of zero, so what they hold is not described by the field list
    at all. It sits inline right after it, as more blocks of the same shape as
    this one but without the FFFF header: a type name, a field count, a field
    list, and possibly nests of its own. An AudioComponent on the fourth
    object of graymane_camp_lv02_after is what this was missing.
    """
    if depth > 16:
        return None
    got = read_fields(d, o)
    if not got:
        return None
    blk = Block(o, type_name, 0)
    blk.fields, o = got
    while True:
        g = pstring(d, o)
        if not g:
            break
        s, nxt = g
        if s.lower().endswith(".prefab"):
            blk.prefab = s
            o = nxt
            break
        sub = read_body(d, nxt, s, depth + 1)
        if not sub:
            break
        blk.children.append(sub[0])
        o = sub[1]
    return blk, o


def read_schema(d):
    """Every top-level block, in file order.

    Only the first object of a type carries the FFFF header and the type name.
    The ones after it start straight in with a field count, so once a type is
    known the walk keeps taking bodies for as long as they look like the same
    thing: a field list whose first field has the same name.
    """
    blocks = []
    n = len(d)
    o = 0x20
    while o + 24 < n:
        if d[o:o + 2] != MARKER:
            o += 1
            continue
        value_size = u16(d, o + 18)
        got = pstring(d, o + 20)
        if not got or not got[0][:1].isalpha():
            o += 1
            continue
        type_name, p = got
        first = read_body(d, p, type_name)
        if not first or not first[0].fields:
            o += 1
            continue
        blk, o = first
        blk.value_size = value_size
        blocks.append(blk)
        anchor = blk.fields[0].name
        while o + 2 < n:
            more = read_body(d, o, type_name)
            if not more or not more[0].fields:
                break
            if more[0].fields[0].name != anchor:
                break
            more[0].value_size = value_size
            blocks.append(more[0])
            o = more[1]
    return blocks


def quat_at(d, o):
    if o + 16 > len(d):
        return None
    q = struct.unpack_from("<ffff", d, o)
    if any(v != v or abs(v) > 1.001 for v in q):
        return None
    if not 0.99 < sum(v * v for v in q) < 1.01:
        return None
    return q


def quat_pos_at(d, o):
    """A unit quaternion followed by a position, with no scale after it."""
    q = quat_at(d, o)
    if q is None or o + 28 > len(d):
        return None
    p = struct.unpack_from("<fff", d, o + 16)
    if any(v != v or abs(v) > 1e7 for v in p):
        return None
    return p


def transform_at(d, o):
    """quaternion, position, scale, or None."""
    q = quat_at(d, o)
    if q is None or o + 40 > len(d):
        return None
    p = struct.unpack_from("<fff", d, o + 16)
    s = struct.unpack_from("<fff", d, o + 28)
    if any(v != v or abs(v) > 1e7 for v in p):
        return None
    if any(v != v or not (0.0001 < v < 10000.0) for v in s):
        return None
    return q, p, s


def transform_runs(d, start=0):
    """Every Transform-shaped run: a unit quaternion, a position, a scale.

    Confirmed against abyssruins_cd_0002.palevel, which declares one placed
    object and yields exactly one run at 0x173C: quaternion (0, 0.997, 0,
    0.078), position (-7035.592, 588.141, -1954.682), scale (1, 1, 1). That
    position sits inside the level's own _minVerts/_maxVerts box, which reads
    (-7039.508, 587.924, -1958.450) to (-7031.654, 589.050, -1950.725), so the
    layout is settled: 16 bytes of quaternion, 12 of position, 12 of scale.

    The `_tiledTransform` follows at +40 and holds the same point with the
    level's tile origin taken off. In that file the origin is
    (-7000, 0, -1000); in graymane_camp_lv02_after it is (-10000, 0, -4000).
    """
    out = []
    o = start
    n = len(d)
    while o + 40 <= n:
        q = struct.unpack_from("<ffff", d, o)
        if all(v == v and abs(v) <= 1.001 for v in q) and 0.99 < sum(v * v for v in q) < 1.01:
            pos = struct.unpack_from("<fff", d, o + 16)
            sc = struct.unpack_from("<fff", d, o + 28)
            if (all(v == v and abs(v) < 1e7 for v in pos)
                    and all(v == v and 0.0001 < v < 10000.0 for v in sc)):
                tiled = None
                if o + 80 <= n:
                    q2 = struct.unpack_from("<ffff", d, o + 40)
                    if all(v == v and abs(v) <= 1.001 for v in q2) and 0.99 < sum(v * v for v in q2) < 1.01:
                        tiled = struct.unpack_from("<fff", d, o + 56)
                out.append((o, pos, sc, tiled))
                o += 40
                continue
        o += 4
    return out


def documents(d):
    """Each PARC sub-document: its schema blocks and where its values start.

    A .palevel is a run of PARC documents. Each opens with the magic, then a
    block header whose u16 at +18 says how many schema blocks follow, then
    that many blocks of `identity string, field count, field list`. The
    identity is a type name for a plain object and a prefab path for a placed
    one, and it comes BEFORE the field list. Whatever follows the last block
    is that document's values.

    Counts verified: abyssruins_cd_0002 declares 10 and yields 10,
    cd_waterfall_cave_0002 declares 40 and yields 40, and
    graymane_camp_lv02_after declares 241 and yields 241.
    """
    out = []
    at = 0
    n = len(d)
    while True:
        k = d.find(b"PARC", at)
        if k < 0:
            break
        h = d.find(MARKER, k, k + 64)
        if h < 0:
            at = k + 4
            continue
        count = u16(d, h + 18)
        o = h + 20
        blocks = []
        ok = True
        for _ in range(count):
            g = pstring(d, o)
            if not g:
                ok = False
                break
            got = read_fields(d, g[1])
            if not got:
                ok = False
                break
            blk = Block(o, g[0], 0)
            blk.fields = got[0]
            blocks.append(blk)
            o = got[1]
        if ok and blocks:
            out.append((k, blocks, o))
        at = o if ok and blocks else k + 4
    return out


def string_table(d, o):
    """The value region opens with an indexed string table, which is what an
    IndexedStringA field indexes into and why it occupies one byte rather than
    holding text. Returns where the table ends."""
    n = len(d)
    while o + 8 <= n:
        g = pstring(d, o + 4)
        if not g:
            break
        o = g[1]
    return o


def placements(d):
    """(prefab, world position, tile origin, offset), in document order.

    Association is by order and anchored on the data rather than computed from
    field offsets. The value records are laid out in schema order, so walking
    the blocks that declare a `_worldTransform` and taking the next Transform
    that actually validates keeps names and positions in step without needing
    every record's exact width, which the field sizes do not give: a
    SceneObject's fields sum to 119 bytes while its records are wider, because
    strings are indices into the table and some fields carry data the sizes do
    not describe.

    A block whose transform does not turn up inside `window` bytes is reported
    as unplaced rather than paired with the next one along.
    """
    rows = []
    unplaced = 0
    for _, blocks, vstart in documents(d):
        # The value region opens with the string table, whose end is not
        # written down. Rather than guess it, the first block's transform is
        # looked for across the whole region and the rest follow it in step.
        o = vstart
        first = True
        for blk in blocks:
            if not blk.field("_worldTransform"):
                continue
            found = None
            # Objects sit far apart in the value region, each followed by its
            # components, so the scan is not windowed. Blocks and transforms are
            # both in file order, so taking the next one keeps them in step.
            limit = len(d)
            # A world transform is the one with a tiled twin forty bytes on
            # whose position is the same point with the level's tile origin
            # taken off. Components carry `_offsetTransform`s of their own that
            # are the same shape, so without that test the walk picks up a
            # mesh's local offset and calls it a placement.
            p = o
            while p + 80 <= limit:
                t = transform_at(d, p)
                if t:
                    # TiledTransform is 44 bytes and does not end in a scale
                    # the way Transform does, so the twin is checked on its
                    # quaternion and position only.
                    tiled = quat_pos_at(d, p + 40)
                    if tiled:
                        dx = t[1][0] - tiled[0] if False else t[1][0] - tiled[0]
                        dz = t[1][2] - tiled[2]
                        if abs(dx) > 1.0 or abs(dz) > 1.0:
                            found = (p, t[1], tiled)
                            break
                p += 4
            if not found:
                unplaced += 1
                continue
            off, pos, tpos = found
            org = None
            if tpos:
                org = (pos[0] - tpos[0], pos[1] - tpos[1], pos[2] - tpos[2])
            rows.append((blk.type, pos, org, off))
            o = off + (80 if tpos else 40)
            first = False
    return rows, unplaced


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--schema", action="store_true")
    ap.add_argument("--types", action="store_true")
    ap.add_argument("--positions", action="store_true")
    ap.add_argument("--grep", default=None)
    a = ap.parse_args()

    d = io.open(a.path, "rb").read()
    if d[:4] != b"PARC":
        print("not a PARC file: %r" % d[:4])
        return 1
    blocks = read_schema(d)
    print("%s: %d bytes, %d declared block(s)" % (a.path, len(d), len(blocks)))

    if a.schema:
        for b in blocks[:3]:
            print("block at 0x%06X  %s  values %d bytes  %d field(s)"
                  % (b.at, b.type, b.value_size, len(b.fields)))
            for f in b.fields:
                print("    +%-4d %-4d %-34s %s" % (f.offset, f.size, f.name, f.type))
            if b.prefab:
                print("    prefab %s" % b.prefab)
        tally = {}
        for b in blocks:
            tally[b.type] = tally.get(b.type, 0) + 1
        print("block types:")
        for k in sorted(tally, key=lambda x: -tally[x]):
            print("    %-30s %d" % (k, tally[k]))

    if a.types:
        n = 0
        for b in blocks:
            if not b.prefab:
                continue
            if a.grep and a.grep.lower() not in b.prefab.lower():
                continue
            print("%4d  %s" % (n, b.prefab))
            n += 1
        print("%d prefab(s)" % n)

    if a.positions:
        rows, unplaced = placements(d)
        print("%d placement(s), %d block(s) whose transform was not found" % (len(rows), unplaced))
        for name, pos, org, off in rows:
            if a.grep and (not name or a.grep.lower() not in name.lower()):
                continue
            print("  0x%06X  (%11.3f, %9.3f, %11.3f)  %s%s"
                  % (off, pos[0], pos[1], pos[2],
                     (name or "(unpaired)").rsplit("/", 1)[-1],
                     ("   tile origin (%.0f, %.0f, %.0f)" % org) if org else ""))

    return 0


if __name__ == "__main__":
    sys.exit(main())
