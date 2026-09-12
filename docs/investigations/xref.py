"""Find RIP-relative references to an RVA anywhere in CrimsonDesert.exe.

Usage: python xref.py <target rva hex> [more rvas...] [--all]

Master Looter's qref.py finds 8-byte pointers, which covers vtables and data
tables and nothing else. Most references in this image are RIP-relative: the
lea that loads a string or a vtable address, and the call rel32 that reaches a
function. Both store a 32-bit displacement at some file position p such that

    target = rva(p) + 4 + displacement

so for a wanted target the displacement is fully determined by where it sits.
This scans every byte position for that value, which is why it needs numpy: it
is a few array operations over the whole 380 MB image instead of 380 million
Python comparisons.

Hits are then filtered against the .pdata RUNTIME_FUNCTION table and only those
landing inside a real function are printed, which throws away the coincidental
matches in data. Pass --all to see the unfiltered hits too.

numpy is a system install here, not vendored the way Master Looter vendors
its own dependencies, so this script does not touch PYTHONPATH the way disasm.py
does. If it is ever missing: py -3 -m pip install numpy

Written 10 September 2026. It found the whole chain from the map icon create
path up to the UI control that owns it: vtable to constructor to factory to the
registry lookup, four steps, one command each.
"""
import bisect
import struct
import sys

import numpy as np

EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IMAGE_BASE = 0x140000000

data = open(EXE, "rb").read()
buf = np.frombuffer(data, dtype=np.uint8)

_pe = struct.unpack_from("<I", data, 0x3C)[0]
_nsec = struct.unpack_from("<H", data, _pe + 6)[0]
_optsz = struct.unpack_from("<H", data, _pe + 20)[0]
_opt = _pe + 24
_magic = struct.unpack_from("<H", data, _opt)[0]
# Data directories start at 0x70 (PE32+) / 0x60 (PE32) into the optional header,
# and directory 3 is the exception directory.
_dd = _opt + (0x70 if _magic == 0x20B else 0x60)
EXC_RVA, EXC_SIZE = struct.unpack_from("<II", data, _dd + 3 * 8)

SECS = []
_tbl = _opt + _optsz
for _i in range(_nsec):
    _e = _tbl + _i * 40
    _name = data[_e:_e + 8].rstrip(b"\0").decode("ascii", "replace")
    _vsize, _vaddr, _rsize, _raddr = struct.unpack_from("<IIII", data, _e + 8)
    SECS.append((_name, _vaddr, _vsize, _raddr, _rsize))


def r2o(rva):
    for name, vaddr, vsize, raddr, rsize in SECS:
        if vaddr <= rva < vaddr + rsize:
            return raddr + (rva - vaddr)
    return None


def o2r(off):
    for name, vaddr, vsize, raddr, rsize in SECS:
        if raddr <= off < raddr + rsize:
            return vaddr + (off - raddr), name
    return None, None


# Function bounds from .pdata, sorted once so lookups are a bisect.
_po = r2o(EXC_RVA)
_nf = EXC_SIZE // 12
_recs = np.frombuffer(data, dtype="<u4", count=_nf * 3, offset=_po)
_starts = _recs[0::3]
_ends = _recs[1::3]
_order = np.argsort(_starts)
STARTS = _starts[_order]
ENDS = _ends[_order]
STARTS_LIST = STARTS.tolist()


def func_of(rva):
    """The function containing an RVA, as (begin, end), or (None, None)."""
    i = bisect.bisect_right(STARTS_LIST, rva) - 1
    if i >= 0 and rva < int(ENDS[i]):
        return int(STARTS[i]), int(ENDS[i])
    return None, None


# The 32-bit little-endian value at every byte position, built once and reused
# for every target on the command line.
VALS = (buf[0:-3].astype(np.uint32)
        | (buf[1:-2].astype(np.uint32) << 8)
        | (buf[2:-1].astype(np.uint32) << 16)
        | (buf[3:].astype(np.uint32) << 24))

# Only the sections big enough to hold code are worth scanning. In this image
# the section names lie: executable code lives in .data2.
BIG = [(n, va, ra, rs) for n, va, vs, ra, rs in SECS if rs > 0x1000000]


def find(target, show_all=False, limit=60):
    hits = 0
    other = 0
    for name, vaddr, raddr, rsize in BIG:
        seg = VALS[raddr:raddr + rsize]
        pos = np.arange(raddr, raddr + len(seg), dtype=np.uint32)
        want = (np.uint32(target) - (np.uint32(vaddr - raddr) + pos) - np.uint32(4))
        for j in np.nonzero(seg == want.astype(np.uint32))[0]:
            off = raddr + int(j)
            rva, sec = o2r(off)
            begin, end = func_of(rva)
            if begin is None:
                other += 1
                if show_all:
                    print("   loose  at RVA 0x%08X (%s)" % (rva, sec))
                continue
            hits += 1
            if hits <= limit:
                print("   disp32 at RVA 0x%08X (%s) inside function 0x%08X..0x%08X  +0x%X"
                      % (rva, sec, begin, end, rva - begin))
            elif hits == limit + 1:
                print("   ... more, raise limit to see them")
    if hits == 0:
        print("   none inside any .pdata function")
    if other and not show_all:
        print("   (%d further match(es) outside any function, pass --all to list)" % other)


def main(argv):
    show_all = "--all" in argv
    targets = [a for a in argv if not a.startswith("--")]
    if not targets:
        print("usage: python xref.py <target rva hex> [more rvas...] [--all]")
        return 1
    for arg in targets:
        target = int(arg, 16)
        print("=== references to RVA 0x%08X ===" % target)
        find(target, show_all)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
