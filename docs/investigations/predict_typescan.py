"""Predict what typescan.cpp will find, by running the same two passes on the file."""
import struct
EXE=r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
d=open(EXE,'rb').read()
pe=struct.unpack_from('<I',d,0x3C)[0]
nsec=struct.unpack_from('<H',d,pe+6)[0]; optsz=struct.unpack_from('<H',d,pe+20)[0]
opt=pe+24
size_of_image=struct.unpack_from('<I',d,opt+56)[0]
secs=[]
tbl=opt+optsz
for i in range(nsec):
    e=tbl+i*40
    nm=d[e:e+8].rstrip(b'\0').decode('ascii','replace')
    vs,va,rs,ra=struct.unpack_from('<IIII',d,e+8); secs.append((nm,va,vs,ra,rs))
def r2o(rva):
    for nm,va,vs,ra,rs in secs:
        if va<=rva<va+rs: return ra+(rva-va)
    return None
KW=("MapIcon","MiniMap","Minimap","WorldMap","DetectMode","SpecialMode")
print("SizeOfImage 0x%X (%d MB)"%(size_of_image,size_of_image//1048576))

# pass one: locators
cols={}
for nm,va,vs,ra,rs in secs:
    for off in range(ra, ra+rs-24, 4):
        if struct.unpack_from('<I',d,off)[0]!=1: continue
        rva=va+(off-ra)
        if struct.unpack_from('<I',d,off+0x14)[0]!=rva: continue
        td=struct.unpack_from('<I',d,off+0x0C)[0]
        if td==0 or td>=size_of_image: continue
        no=r2o(td+0x10)
        if no is None: continue
        end=d.find(b'\0',no,no+300)
        if end<0: continue
        name=d[no:end].decode('ascii','ignore')
        if not name.startswith('.?'): continue
        if any(k in name for k in KW): cols[rva]=name
print("pass one: %d locator(s) matched the keywords"%len(cols))

# pass two: vtables
vt={}
want={0x140000000+r for r in cols}
for nm,va,vs,ra,rs in secs:
    for off in range(ra, ra+rs-8, 8):
        v=struct.unpack_from('<Q',d,off)[0]
        if v in want:
            colrva=v-0x140000000
            if colrva not in vt: vt[colrva]=va+(off-ra)+8
print("pass two: %d of them have a vtable\n"%len(vt))
for rva,name in sorted(cols.items(), key=lambda kv: kv[1]):
    v=vt.get(rva)
    print("  %-12s %s"%(("+0x%08X"%v) if v else "no vtable", name[4:]))
