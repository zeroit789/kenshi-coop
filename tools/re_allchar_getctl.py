# -*- coding: utf-8 -*-
# Para CADA vtable de Character (todas las que referencien un COL con TD=Character),
# resuelve slot +0x1D8 (think real, siguiendo thunk) y +0x58 (getController, siguiendo thunk)
# y desensambla getController. Objetivo: ver si getController devuelve 'this' (identidad) o
# un sub-objeto. Eso decide si SetControlledChar(char+0x250=PI) activa el gate 0x5CD1E3.
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
def rd(rva): return struct.unpack_from("<I",DATA,r2o(rva))[0]
def thunk(rva):
    seen=0
    while True:
        o=r2o(rva)
        if o is None or DATA[o]!=0xE9 or seen>4: return rva
        rva=rva+5+struct.unpack_from("<i",DATA,o+1)[0]; seen+=1
def dump(rva,label,n=0x50):
    o=r2o(rva); code=DATA[o:o+n]; dec=Decoder(64,code,ip=IMAGE_BASE+rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"  -- {label} 0x{rva:X} --")
    for ins in dec:
        rv=ins.ip-IMAGE_BASE; raw=code[ins.ip-(IMAGE_BASE+rva):ins.ip-(IMAGE_BASE+rva)+ins.len]
        print(f"    0x{rv:08X}  {' '.join(f'{b:02x}' for b in raw):<18} {fmt.format(ins)}")
        if ins.mnemonic in (Mnemonic.RET,Mnemonic.INT3): break

needle=b".?AVCharacter@@"; idx=DATA.find(needle); td_rva=o2r(idx-0x10)
# todos los COLs con ese TD
cols=[]
for srva,vs,ro,rs in SEC:
    for off in range(ro,ro+rs-4,4):
        if struct.unpack_from("<I",DATA,off)[0]==td_rva:
            crva=o2r(off)-0xC
            try:
                if rd(crva) in (0,1) and rd(crva+0xC)==td_rva: cols.append(crva)
            except: pass
print("COLs:",[hex(c) for c in cols])
for col in cols:
    col_va=IMAGE_BASE+col
    for srva,vs,ro,rs in SEC:
        for off in range(ro,ro+rs-8,8):
            if struct.unpack_from("<Q",DATA,off)[0]==col_va:
                vtbl=o2r(off)+8
                think=thunk(rq(vtbl+0x1D8)-IMAGE_BASE)
                getctl_raw=rq(vtbl+0x58)-IMAGE_BASE
                getctl=thunk(getctl_raw)
                print(f"\nvtable 0x{vtbl:X} (COL 0x{col:X}): think(+0x1D8)=0x{think:X}  getCtl(+0x58)=0x{getctl:X}")
                dump(getctl, "getController")
                break
