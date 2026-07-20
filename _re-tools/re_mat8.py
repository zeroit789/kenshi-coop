# -*- coding: utf-8 -*-
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
EXE=r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"; IB=0x140000000
DATA=open(EXE,"rb").read()
e=struct.unpack_from("<I",DATA,0x3C)[0]; coff=e+4
ns=struct.unpack_from("<H",DATA,coff+2)[0]; osz=struct.unpack_from("<H",DATA,coff+16)[0]
so=coff+20+osz; SEC=[]
for i in range(ns):
    o=so+i*40; nm=DATA[o:o+8].rstrip(b"\x00").decode("ascii","ignore")
    vs=struct.unpack_from("<I",DATA,o+8)[0]; rv=struct.unpack_from("<I",DATA,o+12)[0]
    rs=struct.unpack_from("<I",DATA,o+16)[0]; ro=struct.unpack_from("<I",DATA,o+20)[0]
    SEC.append((nm,rv,vs,ro,rs))
def r2o(r):
    for nm,sr,vs,ro,rs in SEC:
        if sr<=r<sr+max(vs,rs) and r-sr<rs: return ro+(r-sr)
    return None
def qword(r):
    o=r2o(r); return struct.unpack_from("<Q",DATA,o)[0] if o is not None else None
def rtti_name_from_vtable(vt_rva):
    # vtable[-8] = COL rva (sig=1, x64). COL+0xC = TD rva. TD+0x10 = mangled name string
    col_va=qword(vt_rva-8)
    if col_va is None: return None
    col_rva=col_va-IB
    o=r2o(col_rva)
    if o is None: return None
    # COL: sig@0, offset@4, cdOffset@8, TD rva@0xC (relative to image base for sig=1)
    td_rva=struct.unpack_from("<I",DATA,o+0xC)[0]
    to=r2o(td_rva)
    if to is None: return None
    # TD: vtable@0, spare@8, name@0x10
    raw=DATA[to+0x10:to+0x10+80]
    return raw.split(b"\x00")[0].decode("ascii","ignore")

# vtables conocidas
for name,vt in [("AnimationClass? (char+0x448 vt)",0x16F8A68),  # nota: doc dice CharBody=0x648 vt 0x16F8A68
                ("char+0x448 candidate 0x16FCC88(CharMovement)",0x16FCC88),
                ("AITaskSystem 0x16E3F30",0x16E3F30),
                ("CharBody 0x16F8A68",0x16F8A68)]:
    print(f"{name}: vtable 0x{vt:X} RTTI = {rtti_name_from_vtable(vt)}")

# Ahora: leer la vtable de Character (0x16F9EB8) y resolver slot +0x58 (getFaction), +0x60, +0x40, +0xE8, +0x1D8, +0x1E0
print("\n-- Slots vtable Character 0x16F9EB8 (resolviendo thunk JMP) --")
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
