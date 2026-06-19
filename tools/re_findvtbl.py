# -*- coding: utf-8 -*-
# Busca la vtable cuyo slot +0x1D8 == 0x5CE020 (think) y +0x268 == 0x5CDA20 (move_tick),
# segun el RE del checkpoint. Esa es la vtable REAL del Character en la rama viva 0x5CD1C0.
# Luego resuelve su slot +0x58 (getController) y lo desensambla para ver que devuelve.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic
EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
DATA = open(EXE,"rb").read()
e=struct.unpack_from("<I",DATA,0x3C)[0]; coff=e+4
ns=struct.unpack_from("<H",DATA,coff+2)[0]; osz=struct.unpack_from("<H",DATA,coff+16)[0]
opt=coff+20; so=opt+osz; SEC=[]
for i in range(ns):
    o=so+i*40; SEC.append((struct.unpack_from("<I",DATA,o+12)[0],struct.unpack_from("<I",DATA,o+8)[0],
                           struct.unpack_from("<I",DATA,o+20)[0],struct.unpack_from("<I",DATA,o+16)[0]))
def r2o(rva):
    for srva,vs,ro,rs in SEC:
        if srva<=rva<srva+max(vs,rs) and rva-srva<rs: return ro+(rva-srva)
def o2r(off):
    for srva,vs,ro,rs in SEC:
        if ro<=off<ro+rs: return srva+(off-ro)
def rq(rva): return struct.unpack_from("<Q",DATA,r2o(rva))[0]
def resolve_thunk(rva):
    o=r2o(rva)
    if o is not None and DATA[o]==0xE9:
        return rva+5+struct.unpack_from("<i",DATA,o+1)[0]
    return rva

THINK = IMAGE_BASE + 0x5CE020
MOVE  = IMAGE_BASE + 0x5CDA20
# escanear todas las secciones (vtables suelen estar en .rdata) por qword==THINK
hits=[]
for srva,vs,ro,rs in SEC:
    for off in range(ro, ro+rs-8, 8):
        if struct.unpack_from("<Q",DATA,off)[0]==THINK:
            slot_rva=o2r(off); vtbl=slot_rva-0x1D8
            hits.append(vtbl)
print("vtables con +0x1D8==0x5CE020 (think):", [hex(h) for h in hits])

for vt in hits:
    try:
        m=rq(vt+0x268)-IMAGE_BASE
    except: m=None
    s58=rq(vt+0x58)
    s58_rva=s58-IMAGE_BASE
    real=resolve_thunk(s58_rva)-0  # may be thunk
    real_rva=resolve_thunk(s58_rva)
    print(f"\nvtable 0x{vt:X}: +0x268(move)=0x{m:X if m else 0:X if m else 0}" if m else f"\nvtable 0x{vt:X}: +0x268=?")
    print(f"  +0x58 (getController) raw=0x{s58_rva:X} resuelto=0x{real_rva:X}")
    o=r2o(real_rva); code=DATA[o:o+0x40]; dec=Decoder(64,code,ip=IMAGE_BASE+real_rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    for ins in dec:
        rv=ins.ip-IMAGE_BASE; raw=code[ins.ip-(IMAGE_BASE+real_rva):ins.ip-(IMAGE_BASE+real_rva)+ins.len]
        print(f"    0x{rv:08X}  {' '.join(f'{b:02x}' for b in raw):<18} {fmt.format(ins)}")
        if ins.mnemonic in (Mnemonic.RET,Mnemonic.INT3): break
