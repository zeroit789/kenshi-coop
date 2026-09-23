# ES: Encuentra la función (inicio, fin, tamaño) que contiene un RVA recorriendo el directorio de
#     excepciones (.pdata, entradas RUNTIME_FUNCTION de 12 bytes). Solo lectura.
#     Depende de ke_dis.load()/ke_dis._image, que el ke_dis.py versionado no define.
#     Uso: python ke_find.py <rva_hex>
# EN: Finds the function (start, end, size) containing an RVA by walking the exception directory
#     (.pdata, 12-byte RUNTIME_FUNCTION entries). Read-only.
#     Depends on ke_dis.load()/ke_dis._image, which the versioned ke_dis.py does not define.
#     Usage: python ke_find.py <rva_hex>

# encuentra inicio de funcion via .pdata (RUNTIME_FUNCTION) - READ ONLY
# EN: finds a function start through .pdata (RUNTIME_FUNCTION) - READ ONLY
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
