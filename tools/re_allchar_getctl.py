# -*- coding: utf-8 -*-
# ES: Para cada vtable de Character (todas las que apuntan a un COL cuyo TypeDescriptor es Character)
#     resuelve los slots +0x1D8 (think) y +0x58 (getController) siguiendo thunks y desensambla
#     getController, para ver si devuelve "this" o un subobjeto (decide si SetControlledChar activa el
#     gate 0x5CD1E3). Uso: python re_allchar_getctl.py
# EN: For each Character vtable (all those pointing to a COL whose TypeDescriptor is Character) resolves
#     slots +0x1D8 (think) and +0x58 (getController) following thunks and disassembles getController, to
#     see whether it returns "this" or a subobject (decides whether SetControlledChar triggers gate
#     0x5CD1E3). Usage: python re_allchar_getctl.py

# Para CADA vtable de Character (todas las que referencien un COL con TD=Character),
# resuelve slot +0x1D8 (think real, siguiendo thunk) y +0x58 (getController, siguiendo thunk)
# y desensambla getController. Objetivo: ver si getController devuelve 'this' (identidad) o
# un sub-objeto. Eso decide si SetControlledChar(char+0x250=PI) activa el gate 0x5CD1E3.
# EN: For EACH Character vtable (all those referencing a COL with TD=Character),
#     resolve slot +0x1D8 (real think, following the thunk) and +0x58 (getController, following the thunk)
#     and disassemble getController. Goal: see whether getController returns 'this' (identity) or
#     a subobject. That decides whether SetControlledChar(char+0x250=PI) triggers gate 0x5CD1E3.
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
# ES: Sigue hasta 5 thunks E9 encadenados y devuelve el destino final.
# EN: Follows up to 5 chained E9 thunks and returns the final target.
def thunk(rva):
    seen=0
    while True:
        o=r2o(rva)
        if o is None or DATA[o]!=0xE9 or seen>4: return rva
        rva=rva+5+struct.unpack_from("<i",DATA,o+1)[0]; seen+=1
# ES: Desensambla hasta n bytes desde rva, parando en ret o int3.
# EN: Disassembles up to n bytes from rva, stopping at ret or int3.
def dump(rva,label,n=0x50):
    o=r2o(rva); code=DATA[o:o+n]; dec=Decoder(64,code,ip=IMAGE_BASE+rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"  -- {label} 0x{rva:X} --")
    for ins in dec:
        rv=ins.ip-IMAGE_BASE; raw=code[ins.ip-(IMAGE_BASE+rva):ins.ip-(IMAGE_BASE+rva)+ins.len]
        print(f"    0x{rv:08X}  {' '.join(f'{b:02x}' for b in raw):<18} {fmt.format(ins)}")
        if ins.mnemonic in (Mnemonic.RET,Mnemonic.INT3): break

# ES: TypeDescriptor de Character (0x10 bytes antes del nombre decorado).
# EN: Character TypeDescriptor (0x10 bytes before the mangled name).
needle=b".?AVCharacter@@"; idx=DATA.find(needle); td_rva=o2r(idx-0x10)
# todos los COLs con ese TD
# EN: all COLs with that TD
cols=[]
for srva,vs,ro,rs in SEC:
    for off in range(ro,ro+rs-4,4):
        if struct.unpack_from("<I",DATA,off)[0]==td_rva:
            crva=o2r(off)-0xC
            try:
                if rd(crva) in (0,1) and rd(crva+0xC)==td_rva: cols.append(crva)
            except: pass
print("COLs:",[hex(c) for c in cols])
# ES: Para cada COL, localizar la vtable (qword que apunta al COL + 8) y sus slots.
# EN: For each COL, find the vtable (qword pointing to the COL + 8) and its slots.
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
