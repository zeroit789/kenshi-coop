# -*- coding: utf-8 -*-
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
    return None
def o2r(off):
    for srva,vs,ro,rs in SEC:
        if ro<=off<ro+rs: return srva+(off-ro)
def rq(rva): return struct.unpack_from("<Q",DATA,r2o(rva))[0]
def dump(rva,label,n=0x40):
    o=r2o(rva); code=DATA[o:o+n]; dec=Decoder(64,code,ip=IMAGE_BASE+rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} 0x{rva:X} ===")
    for ins in dec:
        rv=ins.ip-IMAGE_BASE; raw=code[ins.ip-(IMAGE_BASE+rva):ins.ip-(IMAGE_BASE+rva)+ins.len]
        print(f"  0x{rv:08X}  {' '.join(f'{b:02x}' for b in raw):<18} {fmt.format(ins)}")
        if ins.mnemonic in (Mnemonic.RET,Mnemonic.INT3): break

# getController real (destino del thunk de la vtable 0x16F9EB8 slot+0x58)
dump(0x594640, "getController? (vtbl 0x16F9EB8 +0x58 -> thunk -> )")

# La otra vtable de Character (COL 0x183E0C4): hallar su base y su slot +0x58
needle=b".?AVCharacter@@"; idx=DATA.find(needle); td_rva=o2r(idx-0x10)
rd=[s for s in SEC][:]  # rdata es la 2a normalmente; buscar por nombre no guardado, escanear todo
# escanear todo el fichero por qword == VA del COL 0x183E0C4
col_va=IMAGE_BASE+0x183E0C4
for srva,vs,ro,rs in SEC:
    for off in range(ro,ro+rs-8,8):
        if struct.unpack_from("<Q",DATA,off)[0]==col_va:
            vtbl=o2r(off)+8
            s58=rq(vtbl+0x58)-IMAGE_BASE
            print(f"\nvtable B @ 0x{vtbl:X} slot+0x58 -> 0x{s58:X}")
            dump(s58, "getController vtblB +0x58")
            break
