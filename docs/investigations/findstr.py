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

needles = sys.argv[1:]
for s in needles:
    b = s.encode('ascii') + b'\x00'
    idx = 0
    found = []
    while True:
        idx = data.find(b, idx)
        if idx == -1:
            break
        found.append(idx)
        idx += 1
    print("=== %r : %d occurrences ===" % (s, len(found)))
    for off in found[:10]:
        rva, sec = o2r(off)
        print("  file 0x%X -> RVA 0x%X sec %s VA 0x%X" % (off, rva, sec, IMAGE_BASE+rva))
