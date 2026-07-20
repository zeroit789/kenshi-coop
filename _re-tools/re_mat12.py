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
def rtti_from_vt(vt_rva):
    col=qword(vt_rva-8)
    if not col: return None
    o=r2o(col-IB)
    if o is None: return None
    td=struct.unpack_from("<I",DATA,o+0xC)[0]
    to=r2o(td)
    if to is None: return None
    return DATA[to+0x10:to+0x10+80].split(b"\x00")[0].decode("ascii","ignore")
fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
RN={getattr(Register,n):n for n in dir(Register) if isinstance(getattr(Register,n),int) and not n.startswith("_")}
def resolve_thunk(rva,depth=0):
    if depth>4: return rva
    o=r2o(rva)
    if o is None: return rva
    try: ins=next(iter(Decoder(64,DATA[o:o+16],ip=IB+rva)))
    except StopIteration: return rva
    if ins.mnemonic==Mnemonic.JMP and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
        return resolve_thunk(ins.near_branch_target-IB,depth+1)
    return rva

# Desensamblar Character::ctor 0x6223F0 buscando los ctors/escrituras de +0x448, +0x648, +0x650, +0x658
# Foco: ver que vtable se instala en el objeto de char+0x448
o=r2o(0x6223F0); code=DATA[o:o+0x600]
print("=== Character::ctor 0x6223F0 — escrituras a +0x448/+0x648/+0x650/+0x658 y sus ctors ===")
last_call=None
for ins in Decoder(64,code,ip=IB+0x6223F0):
    rr=ins.ip-IB
    if ins.mnemonic in (Mnemonic.CALL,) and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
        last_call=resolve_thunk(ins.near_branch_target-IB)
    if ins.mnemonic==Mnemonic.LEA and ins.op1_kind==OpKind.MEMORY and ins.memory_base==0:
        tgt=ins.memory_displacement-IB
        nm=rtti_from_vt(tgt) if tgt>0 else None
        # registrar lea de posible vtable
        if nm: print(f"  0x{rr:08X}  lea {RN.get(ins.op0_register,'?')}, [0x{tgt:X}]  ; vtable RTTI={nm}")
    if ins.mnemonic==Mnemonic.MOV and ins.op0_kind==OpKind.MEMORY and ins.memory_displacement in (0x448,0x648,0x650,0x658,0x640):
        src = RN.get(ins.op1_register,'imm') if ins.op1_kind==OpKind.REGISTER else 'imm/other'
        print(f"  0x{rr:08X}  mov [char+0x{ins.memory_displacement:X}], {src}   (last ctor call=0x{last_call:X})" if last_call else f"  0x{rr:08X}  mov [char+0x{ins.memory_displacement:X}], {src}")
    if ins.mnemonic==Mnemonic.INT3: break
print("\nNota: en createComponents (no ctor) es donde se alocan los objetos reales. El ctor solo nulifica.")
