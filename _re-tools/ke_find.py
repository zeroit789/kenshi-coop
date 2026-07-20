# encuentra inicio de funcion via .pdata (RUNTIME_FUNCTION) - READ ONLY
import sys, pefile, struct
import ke_dis
pe,_ = ke_dis.load()
EXC = None
for d in pe.OPTIONAL_HEADER.DATA_DIRECTORY:
    if d.name=='IMAGE_DIRECTORY_ENTRY_EXCEPTION':
        EXC=d; break
img = ke_dis._image
base = EXC.VirtualAddress; size = EXC.Size
target = int(sys.argv[1],16)
best=None
for off in range(0, size, 12):
    s,e,u = struct.unpack_from("<III", img, base+off)
    if s==0 and e==0: continue
    if s<=target<e:
        best=(s,e,u); break
if best:
    print(f"func start=0x{best[0]:X} end=0x{best[1]:X} size={best[1]-best[0]} (target 0x{target:X} +0x{target-best[0]:X})")
else:
    print("no pdata entry")
