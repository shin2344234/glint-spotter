"""Dump a vtable slot by slot, and stop where it actually ends.

Usage: python vtdump.py <vtable rva hex> [max slots]

MSVC lays vtables out back to back: the qword before a vtable is its complete
object locator, so the next class's locator is the first qword after this
vtable's last slot. Counting "plausible function pointers" past that point
runs straight into the neighbour's slots. That is how PlayerCameraTPSMode was
believed to have 109 slots when it has sixteen, and how a hook on "slot 19"
landed on PlayerCameraBlackHoleMode's update and never fired.

Each line says which section the slot's target sits in and whether the target
is itself a locator. The first locator is the boundary; the dump stops there
unless --all is given.

Written 10 September 2026, during the pass that found the camera hook was on
the wrong class.
"""
import struct
import sys

EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IMAGE_BASE = 0x140000000

data = open(EXE, "rb").read()


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    tbl = pe + 24 + opt
    out = []
    for i in range(nsec):
        e = tbl + i * 40
        name = d[e:e + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", d, e + 8)
        out.append((name, vaddr, vsize, raddr, rsize))
    return out


SECS = sections(data)


def r2o(rva):
    for name, vaddr, vsize, raddr, rsize in SECS:
        if vaddr <= rva < vaddr + rsize:
            return raddr + (rva - vaddr)
    return None


def sec_of(rva):
    for name, vaddr, vsize, raddr, rsize in SECS:
        if vaddr <= rva < vaddr + rsize:
            return name
    return None


def qword(rva):
    off = r2o(rva)
    return struct.unpack_from("<Q", data, off)[0] if off is not None else None


def dword(rva):
    off = r2o(rva)
    return struct.unpack_from("<I", data, off)[0] if off is not None else None


def is_col(rva):
    # signature 1, and the self-RVA field at +0x14 points back at itself
    if dword(rva) != 1:
        return False
    return dword(rva + 0x14) == rva


def dump(vtable_rva, n=60, stop_at_col=True):
    print("vtable RVA 0x%X" % vtable_rva)
    prev = qword(vtable_rva - 8)
    prev_rva = prev - IMAGE_BASE if prev else 0
    print("  slot[-1] = 0x%X -> %s, locator=%s" % (
        prev or 0, sec_of(prev_rva) if prev else None, is_col(prev_rva) if prev else False))
    for i in range(n):
        rva = vtable_rva + i * 8
        v = qword(rva)
        if v is None:
            print("  slot %2d @ RVA 0x%X : out of mapped range" % (i, rva))
            break
        vrva = v - IMAGE_BASE
        sec = sec_of(vrva) if 0 <= vrva < 0x20000000 else None
        col = is_col(vrva) if sec else False
        if col and stop_at_col:
            print("  slot %2d @ RVA 0x%X : 0x%016X is the next class's locator; the vtable has %d slots" % (i, rva, v, i))
            break
        print("  slot %2d @ RVA 0x%X : 0x%016X  -> rva 0x%08X  %s%s" % (
            i, rva, v, vrva, sec, "  LOCATOR" if col else ""))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    vt = int(sys.argv[1], 16)
    args = [a for a in sys.argv[2:] if a != "--all"]
    n = int(args[0]) if args else 60
    dump(vt, n, stop_at_col="--all" not in sys.argv)
