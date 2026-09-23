# ES: Helper común de la serie k_* (se carga con exec() desde otros scripts): abre kenshi_x64.exe
#     con pefile como imagen mapeada en memoria (data se indexa por RVA), guarda las secciones en
#     secs, define lectores u32/u64, comprobaciones in_text/in_rdata/in_data y carga la tabla .pdata
#     (inicio/fin/unwind de cada función) para func_containing(rva).
# EN: Common helper of the k_* series (loaded via exec() from other scripts): opens kenshi_x64.exe
#     with pefile as a memory-mapped image (data is indexed by RVA), stores sections in secs, defines
#     u32/u64 readers, in_text/in_rdata/in_data checks and loads the .pdata table (start/end/unwind
#     of every function) for func_containing(rva).

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
# ES: Lectores little-endian por RVA y pertenencia a secciones.
# EN: Little-endian readers by RVA and section membership.
def u32(rva): return struct.unpack_from("<I", data, rva)[0]
def u64(rva): return struct.unpack_from("<Q", data, rva)[0]
def in_text(rva): return TEXT_RVA <= rva < TEXT_RVA+TEXT_SZ
def in_rdata(rva): return RDATA_RVA <= rva < RDATA_RVA+RDATA_SZ
def in_data(rva): return DATA_RVA <= rva < DATA_RVA+DATA_SZ
# ES: Lee las entradas RUNTIME_FUNCTION de .pdata (12 bytes: inicio, fin, unwind).
# EN: Reads the .pdata RUNTIME_FUNCTION entries (12 bytes: begin, end, unwind).
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
# ES: Devuelve (inicio, fin) de la función que contiene rva (búsqueda binaria), o None.
# EN: Returns (begin, end) of the function containing rva (binary search), or None.
def func_containing(rva):
    i = bisect.bisect_right(PD_BEGINS, rva)-1
    if i>=0:
        b,e,u = PDATA[i]
        if b<=rva<e: return (b,e)
    return None
