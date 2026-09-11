import struct, sys
import numpy as np
EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IMAGE_BASE = 0x140000000
data = open(EXE, "rb").read()
buf = np.frombuffer(data, dtype=np.uint8)

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

def o2r(off):
    for name, vaddr, vsize, raddr, rsize in SECS:
        if raddr <= off < raddr + rsize:
            return vaddr + (off - raddr), name
    return None, None

def dword(rva):
    off = r2o(rva)
    return struct.unpack_from("<I", data, off)[0] if off is not None else None

def qword(rva):
    off = r2o(rva)
    return struct.unpack_from("<Q", data, off)[0] if off is not None else None

# find all 4-byte aligned positions in file where uint32 == target
def find_dword_hits(target):
    view = buf.view('<u4')
    # only works if len(data)%4==0 sufficiently; use frombuffer on raw bytes with stride
    n = len(data)
    arr = np.frombuffer(data[:n - (n%4)], dtype='<u4')
    idxs = np.nonzero(arr == target)[0]
    return idxs * 4  # file offsets, 4-aligned

def main():
    target_rva = int(sys.argv[1], 16)
    print("searching for COL referencing type descriptor RVA 0x%X" % target_rva)
    hits = find_dword_hits(target_rva)
    print("raw dword hits: %d" % len(hits))
    found = []
    for off in hits:
        rva, sec = o2r(off)
        if rva is None:
            continue
        col_rva = rva - 0x0C
        if dword(col_rva) != 1:
            continue
        self_rva = dword(col_rva + 0x14)
        if self_rva != col_rva:
            continue
        found.append(col_rva)
    print("COL candidates: %d" % len(found))
    for col_rva in found:
        print("  COL at RVA 0x%X (VA 0x%X)" % (col_rva, IMAGE_BASE+col_rva))
        # vtable is right after: find pointer to (col VA) => qword pointing at col_rva+IMAGE_BASE, located right before vtable[0]
    return found

if __name__ == "__main__":
    main()
