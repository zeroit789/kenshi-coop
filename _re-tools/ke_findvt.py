import pefile, struct
EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IB = 0x140000000
pe = pefile.PE(EXE, fast_load=True)
img = pe.get_memory_mapped_image()
size = len(img)
def u32(rva): 
    try: return struct.unpack("<I",img[rva:rva+4])[0]
    except: return None
def u64(rva): 
    try: return struct.unpack("<Q",img[rva:rva+8])[0]
    except: return None
def name_of_vtable(vt_rva):
    try:
        col = u64(vt_rva-8) - IB
        td_rva = u32(col+0xC)
        name=b""; p=td_rva+0x10
        while img[p]!=0 and len(name)<200: name+=bytes([img[p]]); p+=1
        return name.decode('ascii','replace')
    except: return None
# .rdata range
rd_start=None; rd_end=None
for s in pe.sections:
    nm=s.Name.rstrip(b'\x00')
    if nm==b'.rdata':
        rd_start=s.VirtualAddress; rd_end=s.VirtualAddress+s.Misc_VirtualSize
# escanear cada RVA cuyo bajo 16 bits == 0x6338, verificar si es vtable con COL+TD valido
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
