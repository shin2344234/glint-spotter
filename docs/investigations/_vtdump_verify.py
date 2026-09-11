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
    if dword(rva) != 1:
        return False
    return dword(rva + 0x14) == rva


def dump(vtable_rva, n=60):
    print("dump of vtable RVA 0x%X, slot[-1]:" % vtable_rva)
    prev = qword(vtable_rva - 8)
    print("  slot[-1] = 0x%X -> %s (is_col=%s)" % (prev, sec_of(prev - IMAGE_BASE) if prev else None, is_col(prev-IMAGE_BASE) if prev else False))
    for i in range(n):
        rva = vtable_rva + i * 8
        v = qword(rva)
        if v is None:
            print("  slot %2d @ RVA 0x%X : out of mapped range" % (i, rva))
            continue
        vrva = v - IMAGE_BASE
        sec = sec_of(vrva) if 0 <= vrva < 0x20000000 else None
        col_flag = is_col(vrva) if sec else False
        print("  slot %2d @ RVA 0x%X : 0x%016X  -> rva 0x%08X sec=%s col=%s" % (i, rva, v, vrva, sec, col_flag))


if __name__ == "__main__":
    vt = int(sys.argv[1], 16)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    dump(vt, n)
