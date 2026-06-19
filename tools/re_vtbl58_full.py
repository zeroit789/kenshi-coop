# -*- coding: utf-8 -*-
# Desensambla COMPLETO el virtual +0x58 del Character (0x594640) hasta el ret, para ver QUE
# devuelve en rax (identidad 'this'? sub-objeto? objeto global?). Decide la semantica del gate.
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
def dump(rva,n,label):
    o=r2o(rva); code=DATA[o:o+n]; dec=Decoder(64,code,ip=IMAGE_BASE+rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"=== {label} 0x{rva:X} ===")
    rets=0
    for ins in dec:
        rv=ins.ip-IMAGE_BASE; raw=code[ins.ip-(IMAGE_BASE+rva):ins.ip-(IMAGE_BASE+rva)+ins.len]
        s=fmt.format(ins); m=""
        if s.endswith(",rcx") or "rcx" in s: m+=" [this=rcx]"
        if "0x250" in s: m+="  <<+0x250"
        print(f"  0x{rv:08X}  {' '.join(f'{b:02x}' for b in raw):<22} {s}{m}")
        if ins.mnemonic==Mnemonic.RET:
            rets+=1
            if rets>=2: break
        if ins.mnemonic==Mnemonic.INT3: break

dump(0x594640, 0x120, "vtbl+0x58 del Character (getController?)")
