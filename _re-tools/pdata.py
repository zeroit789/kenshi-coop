from re_kenshi import *
import struct
# Parsear .pdata (RUNTIME_FUNCTION: 3 x DWORD: Begin, End, UnwindInfo)
pdata_sec = None
for s in pe.sections:
    nm = s.Name.rstrip(b'\x00').decode('latin1')
    if nm == '.pdata':
        pdata_sec = s
pd_rva = pdata_sec.VirtualAddress
pd_size = pdata_sec.Misc_VirtualSize
raw = data[pd_rva: pd_rva+pd_size]
funcs = []
for off in range(0, len(raw)-12+1, 12):
    beg, end, unw = struct.unpack_from('<III', raw, off)
    if beg == 0 and end == 0: continue
    funcs.append((beg, end))
funcs.sort()

def find_func(rva):
    lo, hi = 0, len(funcs)
    while lo < hi:
        mid = (lo+hi)//2
        if funcs[mid][0] <= rva: lo = mid+1
        else: hi = mid
    if lo == 0: return None
    beg, end = funcs[lo-1]
    if beg <= rva < end:
        return (beg, end)
    return (beg, end)  # devolver el mas cercano por debajo

if __name__ == '__main__':
    import sys
    for a in sys.argv[1:]:
        rva = int(a, 16)
        f = find_func(rva)
        if f:
            print(f"0x{rva:X} esta en funcion 0x{f[0]:X}-0x{f[1]:X} (size 0x{f[1]-f[0]:X})")
        else:
            print(f"0x{rva:X} no encontrada en .pdata")
