"""Resolve a mangled-name RVA (as printed by nav_scan.py) all the way to its
vtable RVA: name_rva -> TD (name_rva-0x10) -> COL (dword scan for TD ref,
sanity-checked self-pointer) -> vtable (qword scan for pointer to COL VA,
vtable = that location + 8).

Usage: python name2vt.py <name_rva_hex> [more...]
"""
import struct, sys
EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IB = 0x140000000
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
            return raddr + (rva - vaddr), name
    return None, None

def o2r(off):
    for name, vaddr, vsize, raddr, rsize in SECS:
        if raddr <= off < raddr + rsize:
            return vaddr + (off - raddr), name
    return None, None

def dword(rva):
    off,_ = r2o(rva)
    return struct.unpack_from("<I", data, off)[0] if off is not None else None

def find_dwords(target):
    out = []
    b = struct.pack("<I", target)
    idx = 0
    while True:
        idx = data.find(b, idx)
        if idx == -1: break
        out.append(idx)
        idx += 1
    return out

def find_qwords(target):
    out = []
    b = struct.pack("<Q", target)
    idx = 0
    while True:
        idx = data.find(b, idx)
        if idx == -1: break
        out.append(idx)
        idx += 1
    return out

def main(argv):
    for arg in argv:
        name_rva = int(arg, 16)
        td_rva = name_rva - 0x10
        td_off,_ = r2o(td_rva)
        name_off,_ = r2o(name_rva)
        name = data[name_off:data.find(b"\0", name_off)].decode('ascii','replace')
        print("=== name RVA 0x%X: %s  (TD RVA 0x%X) ===" % (name_rva, name, td_rva))
        cols = []
        for off in find_dwords(td_rva):
            col_off = off - 0x0C  # pTypeDescriptor field is at COL+0x0C
            col_rva, sec = o2r(col_off)
            if col_rva is None: continue
            if dword(col_rva) != 1: continue
            if dword(col_rva + 0x14) != col_rva: continue
            cols.append(col_rva)
        cols = sorted(set(cols))
        if not cols:
            print("  no COL found")
            continue
        for col_rva in cols:
            col_va = IB + col_rva
            print("  COL at RVA 0x%X (VA 0x%X)" % (col_rva, col_va))
            # find pointer(s) to this COL VA -> vtable = ptr_loc + 8
            for off in find_qwords(col_va):
                loc_rva, sec = o2r(off)
                if loc_rva is None: continue
                vt_rva = loc_rva + 8
                print("    -> vtable RVA 0x%X (in %s)" % (vt_rva, sec))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
