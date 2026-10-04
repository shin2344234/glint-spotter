import sys
from off2rva import IMAGE_BASE, data, o2r

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
