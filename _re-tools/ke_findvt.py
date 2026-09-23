# ES: Busca en .rdata las vtables cuyo RVA termina en 0x6338 (16 bits bajos, útil cuando solo se
#     conoce el final de una dirección vista en memoria) y que tienen COL+TypeDescriptor válidos.
#     Uso: python ke_findvt.py
# EN: Searches .rdata for vtables whose RVA ends in 0x6338 (low 16 bits, useful when only the tail of
#     an address seen in memory is known) and that have a valid COL+TypeDescriptor.
#     Usage: python ke_findvt.py

import pefile, struct
EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IB = 0x140000000
pe = pefile.PE(EXE, fast_load=True)
img = pe.get_memory_mapped_image()
size = len(img)
# ES: Lectores little-endian por RVA (None si falla).
# EN: Little-endian readers by RVA (None on failure).
def u32(rva): 
    try: return struct.unpack("<I",img[rva:rva+4])[0]
    except: return None
def u64(rva): 
    try: return struct.unpack("<Q",img[rva:rva+8])[0]
    except: return None
# ES: Nombre RTTI de la vtable (sin validar la firma del COL), o None.
# EN: RTTI name of the vtable (without validating the COL signature), or None.
def name_of_vtable(vt_rva):
    try:
        col = u64(vt_rva-8) - IB
        td_rva = u32(col+0xC)
        name=b""; p=td_rva+0x10
        while img[p]!=0 and len(name)<200: name+=bytes([img[p]]); p+=1
        return name.decode('ascii','replace')
    except: return None
# ES: rango de .rdata
# .rdata range
rd_start=None; rd_end=None
for s in pe.sections:
    nm=s.Name.rstrip(b'\x00')
    if nm==b'.rdata':
        rd_start=s.VirtualAddress; rd_end=s.VirtualAddress+s.Misc_VirtualSize
# escanear cada RVA cuyo bajo 16 bits == 0x6338, verificar si es vtable con COL+TD valido
# EN: scan every RVA whose low 16 bits == 0x6338, check whether it is a vtable with valid COL+TD
results=[]
base = (rd_start & ~0xFFFF) | 0x6338
r = base
if r < rd_start: r += 0x10000
while r < rd_end:
    nm = name_of_vtable(r)
    if nm and nm.startswith('.?A'):
        results.append((r,nm))
    r += 0x10000
for rva,nm in results:
    print(f"0x{rva:X}  {nm}")
print(f"--- {len(results)} vtables terminadas en 6338 ---")
