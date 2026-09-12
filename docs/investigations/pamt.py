"""Read a Crimson Desert .pamt archive table: full paths, sizes, where each
file lives in the .paz set.

The game ships 194 .paz archives, 141 GB, indexed by 36 .pamt tables. The table
itself is not encrypted, so the whole file listing can be read without touching
an archive. That is what this does. Nothing is decrypted and nothing is
extracted.

LAYOUT, derived from 0000/0.pamt and matching the community's write-ups:

    0x00  u32 magic/crc
    0x04  u32 paz_count
    0x08  u32, u32               two more header words
    0x10  paz table              paz_count entries of 12 bytes
          node section           u32 parent, u8 len, len bytes of name.
                                 `parent` is the BYTE OFFSET of the parent node
                                 inside this section, not an index; the root's
                                 parent is 0xFFFFFFFF. A full path is built by
                                 walking parents and joining the segments, which
                                 already carry their own separators, so
                                 "object" + "/" + "00_common" is three nodes.
          file records           20 bytes: node u32, paz_offset u32,
                                 comp_size u32, orig_size u32, flags u32
                                 archive index = flags & 0xFFFF (unverified)
                                 compression  = (flags >> 16) & 0x0F
                                 0 none, 1 DDS split, 2 LZ4, 3 and 4 unknown

The node section's end is not written down anywhere in the header, so the walk
runs while the records stay self-consistent (a length that fits, a printable
name, a parent that points backwards) and stops when they do not.

Usage:
    py -3 pamt.py <file.pamt> --probe            structure and extension census
    py -3 pamt.py <file.pamt> --list [--grep S]  full paths, optionally filtered
    py -3 pamt.py <file.pamt> --files [--grep S] paths with size and archive
    py -3 pamt.py <dir> --all --grep S           every .pamt under a directory
"""
import argparse
import collections
import glob
import io
import os
import struct
import sys


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Table(object):
    def __init__(self, data, path):
        self.data = data
        self.path = path
        self.magic = u32(data, 0)
        self.paz_count = u32(data, 4)
        self.node_start = 0x10 + self.paz_count * 12
        self.nodes = {}        # relative offset -> (parent relative offset, name)
        self.node_end = None

    def read_nodes(self):
        """Walk every tree in the node area.

        There is more than one. The first holds directory segments; a second
        starts right after it with its own 0xFFFFFFFF root and holds file
        names. Between them sits one u32, which reads like a section length.
        Parent offsets restart at zero in each tree, so each is parsed against
        its own base and the trees are kept apart.
        """
        d = self.data
        n = len(d)
        o = self.node_start
        self.sections = []            # (base, {rel: (parent, name)})
        while o + 9 <= n and u32(d, o) == 0xFFFFFFFF:
            base = o
            nodes = {}
            while o + 5 <= n:
                parent = u32(d, o)
                ln = d[o + 4]
                if ln == 0 or o + 5 + ln > n:
                    break
                name = d[o + 5:o + 5 + ln]
                if any(c < 32 or c >= 127 for c in name):
                    break
                rel = o - base
                if parent != 0xFFFFFFFF and parent >= rel:
                    break          # a parent must already have been seen
                nodes[rel] = (parent, name.decode("ascii"))
                o += 5 + ln
            self.sections.append((base, nodes))
            # One word between trees, then the next root, if there is one.
            if o + 8 <= n and u32(d, o + 4) == 0xFFFFFFFF:
                o += 4
            else:
                break
        self.nodes = self.sections[0][1] if self.sections else {}
        self.node_end = o
        return sum(len(s[1]) for s in self.sections)

    def path_of(self, rel, cache={}):
        out = []
        seen = 0
        while rel in self.nodes and seen < 64:
            parent, name = self.nodes[rel]
            out.append(name)
            if parent == 0xFFFFFFFF:
                break
            rel = parent
            seen += 1
        return "".join(reversed(out))

    def files(self):
        """Yield (path, paz_offset, comp_size, orig_size, flags)."""
        d = self.data
        o = self.node_end
        n = len(d)
        while o + 20 <= n:
            node, off, csz, osz, flags = struct.unpack_from("<IIIII", d, o)
            if node in self.nodes:
                yield (self.path_of(node), off, csz, osz, flags)
            o += 20


def probe(t):
    d = t.data
    print("%s" % t.path)
    print("  size %d (0x%X), magic 0x%08X, paz_count %d, nodes start 0x%X"
          % (len(d), len(d), t.magic, t.paz_count, t.node_start))
    n = t.read_nodes()
    print("  %d nodes, section ends 0x%X, %d bytes left for file records (%d records)"
          % (n, t.node_end, len(d) - t.node_end, (len(d) - t.node_end) // 20))
    exts = collections.Counter()
    shown = 0
    for p, off, csz, osz, flags in t.files():
        dot = p.rfind(".")
        if 0 < dot:
            exts[p[dot:].lower()] += 1
        if shown < 8:
            print("  %-70s off=0x%08X comp=%-9d orig=%-9d flags=0x%08X"
                  % (p[-70:], off, csz, osz, flags))
            shown += 1
    print("  extensions:")
    for e, c in exts.most_common(20):
        print("    %-12s %d" % (e, c))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--probe", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--files", action="store_true")
    ap.add_argument("--all", action="store_true", help="every .pamt under a directory")
    ap.add_argument("--grep", default=None)
    ap.add_argument("--limit", type=int, default=40)
    a = ap.parse_args()

    paths = []
    if a.all or os.path.isdir(a.path):
        for root, _, names in os.walk(a.path):
            for nm in names:
                if nm.lower().endswith(".pamt"):
                    paths.append(os.path.join(root, nm))
    else:
        paths = [a.path]

    shown = 0
    for p in sorted(paths):
        t = Table(io.open(p, "rb").read(), p)
        if a.probe:
            probe(t)
            continue
        t.read_nodes()
        if a.list:
            for rel in sorted(t.nodes):
                full = t.path_of(rel)
                if a.grep and a.grep.lower() not in full.lower():
                    continue
                print(full)
                shown += 1
                if a.limit and shown >= a.limit:
                    return
        else:
            for full, off, csz, osz, flags in t.files():
                if a.grep and a.grep.lower() not in full.lower():
                    continue
                print("%-90s %10d bytes  flags=0x%08X  %s"
                      % (full[-90:], osz, flags, os.path.basename(p)))
                shown += 1
                if a.limit and shown >= a.limit:
                    return


if __name__ == "__main__":
    main()
