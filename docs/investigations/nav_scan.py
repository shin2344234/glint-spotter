import re, struct, sys
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

pat = re.compile(rb'\.\?A[VU]([A-Za-z0-9_]*(?:Navigat|Navi|NavMesh|Voxel|Sector|Path)[A-Za-z0-9_]*)@[A-Za-z0-9_@]*@@\x00')
seen = set()
for m in pat.finditer(data):
    off = m.start()
    rva, sec = o2r(off)
    s = m.group(0).rstrip(b'\x00').decode('ascii')
    key = s
    if key in seen: continue
    seen.add(key)
    print("0x%08X  0x%08X  %-10s %s" % (off, rva if rva else 0, sec, s))
print("total unique:", len(seen))
