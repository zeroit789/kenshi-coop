# -*- coding: utf-8 -*-
# ES: Resuelve por RTTI varias vtables conocidas (CharBody, CharMovement candidata, AITaskSystem) y
#     vuelca algunos slots de la vtable de Character (0x16F9EB8: +0x40, +0x58 getFaction, +0x60, +0xD0,
#     +0xE8, +0x1D8, +0x1E0, +0x268, +0x3F0) siguiendo thunks. Uso: python re_mat8.py
# EN: Resolves via RTTI several known vtables (CharBody, CharMovement candidate, AITaskSystem) and dumps
#     some slots of the Character vtable (0x16F9EB8: +0x40, +0x58 getFaction, +0x60, +0xD0, +0xE8,
#     +0x1D8, +0x1E0, +0x268, +0x3F0) following thunks. Usage: python re_mat8.py

import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
EXE=r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"; IB=0x140000000
DATA=open(EXE,"rb").read()
# ES: Tabla de secciones leída a mano de las cabeceras PE (e_lfanew -> COFF -> cabeceras de sección de 40 bytes).
# EN: Section table parsed by hand from the PE headers (e_lfanew -> COFF -> 40-byte section headers).
e=struct.unpack_from("<I",DATA,0x3C)[0]; coff=e+4
ns=struct.unpack_from("<H",DATA,coff+2)[0]; osz=struct.unpack_from("<H",DATA,coff+16)[0]
so=coff+20+osz; SEC=[]
for i in range(ns):
    o=so+i*40; nm=DATA[o:o+8].rstrip(b"\x00").decode("ascii","ignore")
    vs=struct.unpack_from("<I",DATA,o+8)[0]; rv=struct.unpack_from("<I",DATA,o+12)[0]
    rs=struct.unpack_from("<I",DATA,o+16)[0]; ro=struct.unpack_from("<I",DATA,o+20)[0]
    SEC.append((nm,rv,vs,ro,rs))
# ES: RVA -> offset en el fichero (None si el RVA no tiene datos en disco).
# EN: RVA -> file offset (None if the RVA has no on-disk data).
def r2o(r):
    for nm,sr,vs,ro,rs in SEC:
        if sr<=r<sr+max(vs,rs) and r-sr<rs: return ro+(r-sr)
    return None
# ES: Lee un qword (8 bytes little-endian) en un RVA.
# EN: Reads a qword (8 bytes little-endian) at an RVA.
def qword(r):
    o=r2o(r); return struct.unpack_from("<Q",DATA,o)[0] if o is not None else None
# ES: Nombre RTTI de la vtable (ver comentarios internos sobre el layout COL/TD).
# EN: RTTI name of the vtable (see inner comments on the COL/TD layout).
def rtti_name_from_vtable(vt_rva):
    # ES: vtable[-8] = puntero al COL (x64, sig=1). COL+0xC = RVA del TD. TD+0x10 = nombre decorado
    # vtable[-8] = COL rva (sig=1, x64). COL+0xC = TD rva. TD+0x10 = mangled name string
    col_va=qword(vt_rva-8)
    if col_va is None: return None
    col_rva=col_va-IB
    o=r2o(col_rva)
    if o is None: return None
    # ES: COL: firma@0, offset@4, cdOffset@8, RVA del TD@0xC (relativo a la base de imagen si sig=1)
    # COL: sig@0, offset@4, cdOffset@8, TD rva@0xC (relative to image base for sig=1)
    td_rva=struct.unpack_from("<I",DATA,o+0xC)[0]
    to=r2o(td_rva)
    if to is None: return None
    # ES: TD: vtable@0, spare@8, nombre@0x10
    # TD: vtable@0, spare@8, name@0x10
    raw=DATA[to+0x10:to+0x10+80]
    return raw.split(b"\x00")[0].decode("ascii","ignore")

# vtables conocidas
# EN: known vtables (the inline note says: docs say CharBody=0x648, vtable 0x16F8A68)
for name,vt in [("AnimationClass? (char+0x448 vt)",0x16F8A68),  # nota: doc dice CharBody=0x648 vt 0x16F8A68
                ("char+0x448 candidate 0x16FCC88(CharMovement)",0x16FCC88),
                ("AITaskSystem 0x16E3F30",0x16E3F30),
                ("CharBody 0x16F8A68",0x16F8A68)]:
    print(f"{name}: vtable 0x{vt:X} RTTI = {rtti_name_from_vtable(vt)}")

# Ahora: leer la vtable de Character (0x16F9EB8) y resolver slot +0x58 (getFaction), +0x60, +0x40, +0xE8, +0x1D8, +0x1E0
# EN: Now: read the Character vtable (0x16F9EB8) and resolve slots +0x58 (getFaction), +0x60, +0x40, +0xE8, +0x1D8, +0x1E0
print("\n-- Slots vtable Character 0x16F9EB8 (resolviendo thunk JMP) --")
# ES: Si en rva hay un JMP (thunk), lo sigue recursivamente (máx. 4 saltos) hasta la función real.
# EN: If there is a JMP (thunk) at rva, follows it recursively (max 4 hops) to the real function.
def resolve_thunk(rva,depth=0):
    if depth>4: return rva
    o=r2o(rva)
    if o is None: return rva
    try: ins=next(iter(Decoder(64,DATA[o:o+16],ip=IB+rva)))
    except StopIteration: return rva
    if ins.mnemonic==Mnemonic.JMP and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
        return resolve_thunk(ins.near_branch_target-IB,depth+1)
    return rva
VT=0x16F9EB8
for slot in [0x40,0x58,0x60,0xD0,0xE8,0x1D8,0x1E0,0x268,0x3F0]:
    p=qword(VT+slot)
    if p:
        r=p-IB; real=resolve_thunk(r)
        print(f"  vt+0x{slot:<4X} -> 0x{r:X} (real 0x{real:X})")
