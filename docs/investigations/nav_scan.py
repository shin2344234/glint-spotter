import re
from off2rva import data, o2r

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
