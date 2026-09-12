import struct, sys

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

def o2r(off):
    for name, vaddr, vsize, raddr, rsize in SECS:
        if raddr <= off < raddr + rsize:
            return vaddr + (off - raddr), name
    return None, None

def r2o(rva):
    for name, vaddr, vsize, raddr, rsize in SECS:
        if vaddr <= rva < vaddr + rsize:
            return raddr + (rva - vaddr)
    return None

if __name__ == "__main__":
    off = int(sys.argv[1], 16)
    rva, sec = o2r(off)
    print("file offset 0x%X -> RVA 0x%X (section %s) VA 0x%X" % (off, rva, sec, IMAGE_BASE+rva))
    # print surrounding bytes as text
    print(repr(data[off-40:off+80]))
