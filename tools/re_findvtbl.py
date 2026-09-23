# -*- coding: utf-8 -*-
# ES: Busca la vtable cuyo slot +0x1D8 es think (0x5CE020): la vtable real de Character en la rama
#     viva 0x5CD1C0. Después lee +0x268 (move_tick) y resuelve y desensambla +0x58 (getController).
#     Ojo: el f-string que imprime +0x268 tiene un formato raro que probablemente falla al ejecutarse.
#     Uso: python re_findvtbl.py
# EN: Finds the vtable whose +0x1D8 slot is think (0x5CE020): the real Character vtable in the live
#     branch 0x5CD1C0. Then reads +0x268 (move_tick) and resolves and disassembles +0x58 (getController).
#     Note: the f-string printing +0x268 has an odd format spec that probably fails at runtime.
#     Usage: python re_findvtbl.py

# Busca la vtable cuyo slot +0x1D8 == 0x5CE020 (think) y +0x268 == 0x5CDA20 (move_tick),
# segun el RE del checkpoint. Esa es la vtable REAL del Character en la rama viva 0x5CD1C0.
# Luego resuelve su slot +0x58 (getController) y lo desensambla para ver que devuelve.
import struct
# EN: Finds the vtable whose slot +0x1D8 == 0x5CE020 (think) and +0x268 == 0x5CDA20 (move_tick),
#     per the checkpoint RE. That is the REAL Character vtable in the live branch 0x5CD1C0.
#     Then resolves its +0x58 slot (getController) and disassembles it to see what it returns.
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
# ES: Si en la dirección hay un JMP (thunk), devuelve su destino real (según la variante sigue uno o varios saltos).
# EN: If there is a JMP (thunk) at the address, returns its real target (one or several hops depending on the variant).
# ES: Sigue un thunk jmp E9 (un nivel).
# EN: Follows one E9 jmp thunk (one level).
def resolve_thunk(rva):
    o=r2o(rva)
    if o is not None and DATA[o]==0xE9:
        return rva+5+struct.unpack_from("<i",DATA,o+1)[0]
    return rva

THINK = IMAGE_BASE + 0x5CE020
MOVE  = IMAGE_BASE + 0x5CDA20
# escanear todas las secciones (vtables suelen estar en .rdata) por qword==THINK
# EN: scan every section (vtables are usually in .rdata) for qword==THINK
hits=[]
for srva,vs,ro,rs in SEC:
    for off in range(ro, ro+rs-8, 8):
        if struct.unpack_from("<Q",DATA,off)[0]==THINK:
            slot_rva=o2r(off); vtbl=slot_rva-0x1D8
            hits.append(vtbl)
print("vtables con +0x1D8==0x5CE020 (think):", [hex(h) for h in hits])

# ES: Para cada vtable hallada: slots +0x268 y +0x58 y desensamblado de getController.
# EN: For each vtable found: slots +0x268 and +0x58 and getController disassembly.
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
