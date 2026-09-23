# -*- coding: utf-8 -*-
# ES: Examina los dos COL de Character (0x183E0C4 y 0x1842D30): firma, offset (qué subobjeto/base son),
#     localiza cada vtable y desensambla su slot +0x58 (getController). Uso: python re_charvtbl2.py
# EN: Examines both Character COLs (0x183E0C4 and 0x1842D30): signature, offset (which subobject/base they
#     are), finds each vtable and disassembles its +0x58 slot (getController). Usage: python re_charvtbl2.py

# Examina AMBOS COLs de Character, su offset (que base son), localiza cada vtable y su slot+0x58.
# EN: Examines BOTH Character COLs, their offset (which base they are), finds each vtable and its slot+0x58.
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
# ES: Offset de fichero -> RVA.
# EN: File offset -> RVA.
def o2r(off):
    for srva,vs,ro,rs in SEC:
        if ro<=off<ro+rs: return srva+(off-ro)
# ES: Lee un qword (8 bytes) en un RVA.
# EN: Reads a qword (8 bytes) at an RVA.
def rq(rva): return struct.unpack_from("<Q",DATA,r2o(rva))[0]
# ES: Lee un dword (4 bytes) en un RVA.
# EN: Reads a dword (4 bytes) at an RVA.
def rd(rva): return struct.unpack_from("<I",DATA,r2o(rva))[0]
# ES: Si en la dirección hay un JMP (thunk), devuelve su destino real (según la variante sigue uno o varios saltos).
# EN: If there is a JMP (thunk) at the address, returns its real target (one or several hops depending on the variant).
def resolve_thunk(rva):
    # si es 'e9 xx xx xx xx' (jmp rel32) seguir una vez
    # EN: if it is 'e9 xx xx xx xx' (jmp rel32) follow it once
    o=r2o(rva)
    if DATA[o]==0xE9:
        rel=struct.unpack_from("<i",DATA,o+1)[0]; return rva+5+rel
    return rva
# ES: Desensambla (tras resolver un thunk) hasta ret o int3.
# EN: Disassembles (after resolving a thunk) up to ret or int3.
def dump(rva,label,n=0x50):
    rva=resolve_thunk(rva)
    o=r2o(rva); code=DATA[o:o+n]; dec=Decoder(64,code,ip=IMAGE_BASE+rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} (resuelto 0x{rva:X}) ===")
    for ins in dec:
        rv=ins.ip-IMAGE_BASE; raw=code[ins.ip-(IMAGE_BASE+rva):ins.ip-(IMAGE_BASE+rva)+ins.len]
        print(f"  0x{rv:08X}  {' '.join(f'{b:02x}' for b in raw):<18} {fmt.format(ins)}")
        if ins.mnemonic in (Mnemonic.RET,Mnemonic.INT3): break

# ES: Para cada COL: campos y vtable(s) que apuntan a él.
# EN: For each COL: fields and the vtable(s) pointing to it.
for col in (0x183E0C4, 0x1842D30):
    sig=rd(col); off=rd(col+4); cd=rd(col+8); tdp=rd(col+0xC)
    print(f"COL 0x{col:X}: sig={sig} offset={off} cdOffset={cd} TD=0x{tdp:X}")
    col_va=IMAGE_BASE+col
    for srva,vs,ro,rs in SEC:
        for o in range(ro,ro+rs-8,8):
            if struct.unpack_from("<Q",DATA,o)[0]==col_va:
                vtbl=o2r(o)+8; s58=rq(vtbl+0x58)-IMAGE_BASE
                s1d8=rq(vtbl+0x1D8)-IMAGE_BASE; s268=rq(vtbl+0x268)-IMAGE_BASE
                print(f"  vtable @ 0x{vtbl:X}  +0x58=0x{s58:X}  +0x1D8=0x{s1d8:X}  +0x268=0x{s268:X}")
                dump(s58, f"getController vtbl@0x{vtbl:X} +0x58")
                break
