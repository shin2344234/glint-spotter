import struct, sys
import off2rva as m

TD_RVA = int(sys.argv[1], 16)
pat = struct.pack('<I', TD_RVA)
data = m.data
idx = 0
hits = []
while True:
    i = data.find(pat, idx)
    if i == -1: break
    hits.append(i)
    idx = i+1

print(f"raw pattern hits: {len(hits)}")
for i in hits:
    col_off = i - 0xC
    if col_off < 0: continue
    sig = struct.unpack_from('<I', data, col_off)[0]
    if sig != 1: continue
    self_field = struct.unpack_from('<I', data, col_off+0x14)[0]
    col_rva, sec = m.o2r(col_off)
    if col_rva is None: continue
    if self_field != col_rva: continue
    print("COL at file off 0x%X RVA 0x%X (%s), self-check OK" % (col_off, col_rva, sec))
