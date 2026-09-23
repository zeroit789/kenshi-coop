# ES: Versión generalizada de ke_findvt.py: busca vtables de .rdata (alineadas a 8) cuyo RVA termina
#     en el sufijo hexadecimal dado (por defecto 0x6338; la máscara depende del número de dígitos).
#     Uso: python ke_findvt2.py [sufijo_hex]
# EN: Generalized version of ke_findvt.py: searches .rdata vtables (8-aligned) whose RVA ends in the
#     given hex suffix (default 0x6338; the mask depends on the number of digits).
#     Usage: python ke_findvt2.py [suffix_hex]

import pefile, struct, sys
EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IB = 0x140000000
pe = pefile.PE(EXE, fast_load=True)
img = pe.get_memory_mapped_image()
# ES: Lectores little-endian por RVA (None si falla).
# EN: Little-endian readers by RVA (None on failure).
def u32(rva): 
    try: return struct.unpack("<I",img[rva:rva+4])[0]
    except: return None
def u64(rva): 
    try: return struct.unpack("<Q",img[rva:rva+8])[0]
    except: return None
# ES: Nombre RTTI (".?AV...") de la vtable vía COL (firma 0/1) y TypeDescriptor, o None.
# EN: RTTI name (".?AV...") of the vtable through the COL (signature 0/1) and TypeDescriptor, or None.
def name_of_vtable(vt_rva):
    try:
        colp=u64(vt_rva-8)
        if colp is None or colp<IB: return None
        col=colp-IB
        sig=u32(col)
        if sig not in (0,1): return None
        td_rva=u32(col+0xC)
        name=b""; p=td_rva+0x10
        while img[p]!=0 and len(name)<200: name+=bytes([img[p]]); p+=1
        s=name.decode('ascii','replace')
        return s if s.startswith('.?A') else None
    except: return None
rd_start=rd_end=None
for s in pe.sections:
    if s.Name.rstrip(b'\x00')==b'.rdata':
        rd_start=s.VirtualAddress; rd_end=s.VirtualAddress+s.Misc_VirtualSize
suffix = int(sys.argv[1],16) if len(sys.argv)>1 else 0x6338
smask = (1<<(4*len(sys.argv[1].replace('0x','')))) -1 if len(sys.argv)>1 else 0xFFFF
# escanear alineado a 8 (vtables alineadas a 8)
# EN: scan 8-aligned (vtables are 8-aligned)
r = rd_start - (rd_start % 8)
hits=[]
while r < rd_end:
    if (r & smask) == suffix:
        nm = name_of_vtable(r)
        if nm: hits.append((r,nm))
    r += 8
for rva,nm in hits:
    print(f"0x{rva:X}  {nm}")
print(f"--- {len(hits)} matches sufijo 0x{suffix:X} ---")
