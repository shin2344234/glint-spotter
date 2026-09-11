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
frame the mod already handles reports (-9000, 0, -4000) for the area Seth
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


def positions(d):
    """Transform pairs: (offset, world position, tile origin)."""
    out = []
    o = 0
    while o < len(d) - 0x50:
        a = transform_at(d, o)
        if a:
            b = transform_at(d, o + 0x28)
            if b:
                w, t = a[1], b[1]
                dx, dy, dz = w[0] - t[0], w[1] - t[1], w[2] - t[2]
                if abs(dx) > 1.0 or abs(dz) > 1.0:
                    out.append((o, w, (dx, dy, dz)))
                    o += 0x50
                    continue
        o += 4
    return out


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
        ps = positions(d)
        print("%d transform pair(s) found by shape" % len(ps))
        for o, w, org in ps:
            print("  0x%06X  world (%11.3f, %9.3f, %11.3f)   tile origin (%.0f, %.0f, %.0f)"
                  % (o, w[0], w[1], w[2], org[0], org[1], org[2]))
        print("NOTE: this is a shape scan, not a walk of the value region. It is")
        print("incomplete and does not say which object each position belongs to.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
