"""Resolve an RVA that sits inside a vtable back to its class and slot index.

Usage: python vtres.py <rva hex> [more rvas...]

MSVC puts a pointer to the RTTICompleteObjectLocator at vtable[-1], so given any
address inside a vtable you can walk backwards a slot at a time until the qword
before the candidate start points at a locator whose self-RVA field points back
at itself. That check is strict enough that false positives do not happen in
practice, and it gives the class name and the slot index in one step.

Master Looter has a vtable.py that misreads its slot index. Use this instead, or
read slots by hand against a section map.

Written 10 September 2026 while finding which class owns the map icon create
path. It answered that in one call: RVA 0x0555D1C0 turned out to be slot 170 of
pa::uiCommonScript::UIGamePlayControlRootWorldMap.
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
    """RVA to file offset, or None when the RVA is in a zero-filled tail."""
    for name, vaddr, vsize, raddr, rsize in SECS:
        if vaddr <= rva < vaddr + rsize:
            return raddr + (rva - vaddr)
    return None


def qword(rva):
    off = r2o(rva)
    return struct.unpack_from("<Q", data, off)[0] if off is not None else None


def dword(rva):
    off = r2o(rva)
    return struct.unpack_from("<I", data, off)[0] if off is not None else None


def type_name(td_rva):
    """The decorated name out of a TypeDescriptor: { vftable, spare, char name[] }."""
    off = r2o(td_rva)
    if off is None:
        return None
    end = data.find(b"\0", off + 0x10)
    return data[off + 0x10:end].decode("ascii", "replace")


def readable(decorated):
    """pa::uiCommonScript::Foo out of .?AVFoo@uiCommonScript@pa@@, best effort.

    Templates carry ?$ and their own nested argument lists, so those are left
    alone rather than mangled further by a half-parser.
    """
    if not decorated or "?$" in decorated:
        return decorated
    for prefix in (".?AV", ".?AU", ".?AW4"):
        if decorated.startswith(prefix):
            body = decorated[len(prefix):]
            break
    else:
        return decorated
    parts = [p for p in body.split("@") if p]
    if not parts:
        return decorated
    return "::".join(reversed(parts))


def locator_at(rva):
    """Class name if rva holds a plausible RTTICompleteObjectLocator, else None.

    x64 layout: signature, offset, cdOffset, pTypeDescriptor, pClassDescriptor,
    pSelf, all 4 bytes. Signature is 1 and pSelf points back at the locator, and
    requiring both is what keeps this from matching arbitrary data.
    """
    if dword(rva) != 1:
        return None
    if dword(rva + 0x14) != rva:
        return None
    return type_name(dword(rva + 0x0C))


def resolve(rva, max_slots=400):
    for k in range(max_slots):
        vtable = rva - k * 8
        col_va = qword(vtable - 8)
        if col_va is None or not (IMAGE_BASE < col_va < IMAGE_BASE + 0x18000000):
            continue
        name = locator_at(col_va - IMAGE_BASE)
        if name:
            return vtable, k, name, col_va - IMAGE_BASE
    return None, None, None, None


def main(argv):
    if not argv:
        print("usage: python vtres.py <rva hex> [more rvas...]")
        return 1
    for arg in argv:
        rva = int(arg, 16)
        vtable, slot, name, col = resolve(rva)
        if not name:
            print("RVA 0x%08X: no complete object locator within 400 slots" % rva)
            continue
        print("RVA 0x%08X is slot %d of vtable 0x%08X  (vtable offset +0x%X)"
              % (rva, slot, vtable, rva - vtable))
        print("   locator 0x%08X" % col)
        print("   class   %s" % readable(name))
        print("   raw     %s" % name)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
