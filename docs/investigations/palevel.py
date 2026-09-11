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
        self.prefab = None

    def field(self, name):
        for f in self.fields:
            if f.name == name:
                return f
        return None


MARKER = b"\xFF\xFF"


def read_schema(d):
    """Every declared block, in file order."""
    blocks = []
    o = 0x20
    n = len(d)
    while o + 24 < n:
        # A block opens with FFFF, then 18 bytes of header whose last u16 is
        # the total width of the object's values.
        if d[o:o + 2] != MARKER:
            o += 1
            continue
        value_size = u16(d, o + 18)
        got = pstring(d, o + 20)
        if not got:
            o += 1
            continue
        type_name, p = got
        if not type_name[:1].isalpha():
            o += 1
            continue
        if p + 2 > n:
            break
        count = u16(d, p)
        p += 2
        if count == 0 or count > 200:
            o += 1
            continue
        blk = Block(o, type_name, value_size)
        run = 0
        ok = True
        for _ in range(count):
            gf = pstring(d, p)
            if not gf:
                ok = False
                break
            fname, p = gf
            gt = pstring(d, p)
            if not gt:
                ok = False
                break
            tname, p = gt
            if p + 8 > n:
                ok = False
                break
            # u16 kind, u16 size, u32 flags. Transform reads 0x28 here, which
            # is the 40 bytes its quaternion, position and scale occupy.
            size = u16(d, p + 2)
            p += 8
            blk.fields.append(Field(fname, tname, size, run))
            run += size
        if not ok:
            o += 1
            continue
        # The object's own prefab follows the field list, when it has one.
        gp = pstring(d, p)
        if gp and gp[0].lower().endswith(".prefab"):
            blk.prefab = gp[0]
            p = gp[1]
        blocks.append(blk)
        # Only the first object of a type carries the FFFF header and the type
        # name. The ones after it start straight in with a field count, so the
        # walk keeps going here rather than hunting for another marker.
        o = p
        while o + 2 < n:
            count2 = u16(d, o)
            # Objects of the same type can declare different field counts, so
            # the count is only sanity-checked rather than required to match.
            if count2 < 1 or count2 > 200:
                break
            q = o + 2
            blk2 = Block(o, type_name, value_size)
            good = True
            run2 = 0
            for _ in range(count2):
                gf = pstring(d, q)
                if not gf:
                    good = False
                    break
                fname, q = gf
                gt = pstring(d, q)
                if not gt:
                    good = False
                    break
                tname, q = gt
                if q + 8 > n:
                    good = False
                    break
                blk2.fields.append(Field(fname, tname, u16(d, q + 2), run2))
                run2 += u16(d, q + 2)
                q += 8
            if not good or not blk2.fields or not blk2.fields[0].name.startswith("_"):
                break
            gp2 = pstring(d, q)
            if gp2 and gp2[0].lower().endswith(".prefab"):
                blk2.prefab = gp2[0]
                q = gp2[1]
            blocks.append(blk2)
            o = q
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
