import pefile, struct, sys
EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IB = 0x140000000
pe = pefile.PE(EXE, fast_load=True)
img = pe.get_memory_mapped_image()
def u32(rva):
    try: return struct.unpack("<I",img[rva:rva+4])[0]
    except: return None
def u64(rva):
    try: return struct.unpack("<Q",img[rva:rva+8])[0]
    except: return None
def name_of_vtable(vt_rva):
    try:
        colp=u64(vt_rva-8)
        if colp is None or colp<IB: return None
        col=colp-IB; sig=u32(col)
        if sig not in (0,1): return None
        td=u32(col+0xC); name=b""; p=td+0x10
        while img[p]!=0 and len(name)<200: name+=bytes([img[p]]); p+=1
        s=name.decode('ascii','replace')
        return s if s.startswith('.?A') else None
    except: return None
want = sys.argv[1]  # substring del nombre
rd_start=rd_end=None
for s in pe.sections:
    if s.Name.rstrip(b'\x00')==b'.rdata':
        rd_start=s.VirtualAddress; rd_end=s.VirtualAddress+s.Misc_VirtualSize
r=rd_start-(rd_start%8)
while r<rd_end:
    nm=name_of_vtable(r)
    if nm and want in nm:
        print(f"0x{r:X}  (low16=0x{r&0xFFFF:04X})  {nm}")
    r+=8
