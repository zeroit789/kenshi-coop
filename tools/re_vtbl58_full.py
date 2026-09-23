# -*- coding: utf-8 -*-
# ES: Desensambla completo el virtual +0x58 de Character (0x594640) hasta el segundo ret, marcando rcx
#     (this) y +0x250, para ver qué devuelve en rax. Uso: python re_vtbl58_full.py
# EN: Fully disassembles Character's virtual +0x58 (0x594640) up to the second ret, flagging rcx (this)
#     and +0x250, to see what it returns in rax. Usage: python re_vtbl58_full.py

# Desensambla COMPLETO el virtual +0x58 del Character (0x594640) hasta el ret, para ver QUE
# devuelve en rax (identidad 'this'? sub-objeto? objeto global?). Decide la semantica del gate.
# EN: Fully disassembles Character's virtual +0x58 (0x594640) up to ret, to see WHAT it
#     returns in rax (identity 'this'? subobject? global object?). Decides the gate semantics.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic
EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
DATA = open(EXE,"rb").read()
# ES: Tabla de secciones leída a mano de las cabeceras PE (e_lfanew -> COFF -> cabeceras de sección de 40 bytes).
# EN: Section table parsed by hand from the PE headers (e_lfanew -> COFF -> 40-byte section headers).
e=struct.unpack_from("<I",DATA,0x3C)[0]; coff=e+4
ns=struct.unpack_from("<H",DATA,coff+2)[0]; osz=struct.unpack_from("<H",DATA,coff+16)[0]
opt=coff+20; so=opt+osz; SEC=[]
for i in range(ns):
    o=so+i*40; SEC.append((struct.unpack_from("<I",DATA,o+12)[0],struct.unpack_from("<I",DATA,o+8)[0],
                           struct.unpack_from("<I",DATA,o+20)[0],struct.unpack_from("<I",DATA,o+16)[0]))
# ES: RVA -> offset en el fichero (None si el RVA no tiene datos en disco).
# EN: RVA -> file offset (None if the RVA has no on-disk data).
def r2o(rva):
    for srva,vs,ro,rs in SEC:
        if srva<=rva<srva+max(vs,rs) and rva-srva<rs: return ro+(rva-srva)
# ES: Desensambla n bytes marcando this/+0x250; para en el segundo ret o en int3.
# EN: Disassembles n bytes flagging this/+0x250; stops at the second ret or at int3.
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
