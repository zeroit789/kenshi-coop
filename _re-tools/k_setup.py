import pefile, struct, bisect
PATH = r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
pe = pefile.PE(PATH, fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
secs = {}
for s in pe.sections:
    name = s.Name.rstrip(b'\x00').decode('latin1')
    secs[name] = (s.VirtualAddress, s.Misc_VirtualSize)
data = pe.get_memory_mapped_image()
TEXT_RVA, TEXT_SZ = secs['.text']
RDATA_RVA, RDATA_SZ = secs['.rdata']
DATA_RVA, DATA_SZ = secs['.data']
PDATA_RVA, PDATA_SZ = secs['.pdata']
def u32(rva): return struct.unpack_from("<I", data, rva)[0]
def u64(rva): return struct.unpack_from("<Q", data, rva)[0]
def in_text(rva): return TEXT_RVA <= rva < TEXT_RVA+TEXT_SZ
def in_rdata(rva): return RDATA_RVA <= rva < RDATA_RVA+RDATA_SZ
def in_data(rva): return DATA_RVA <= rva < DATA_RVA+DATA_SZ
def pdata_funcs():
    funcs=[]
    n = PDATA_SZ//12
    for i in range(n):
        off = PDATA_RVA + i*12
        begin=u32(off); end=u32(off+4); unwind=u32(off+8)
        if begin==0 and end==0: continue
        funcs.append((begin,end,unwind))
    return funcs
PDATA = pdata_funcs(); PDATA.sort()
PD_BEGINS = [f[0] for f in PDATA]
def func_containing(rva):
    i = bisect.bisect_right(PD_BEGINS, rva)-1
    if i>=0:
        b,e,u = PDATA[i]
        if b<=rva<e: return (b,e)
    return None
