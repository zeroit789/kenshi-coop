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
    td=struct.unpack_from("<I",DATA,o+0xC)[0]; to=r2o(td)
    if to is None: return None
    return DATA[to+0x10:to+0x10+80].split(b"\x00")[0].decode("ascii","ignore")
def resolve_thunk(rva,depth=0):
    if depth>4: return rva
    o=r2o(rva)
    if o is None: return rva
    try: ins=next(iter(Decoder(64,DATA[o:o+16],ip=IB+rva)))
    except StopIteration: return rva
    if ins.mnemonic==Mnemonic.JMP and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
        return resolve_thunk(ins.near_branch_target-IB,depth+1)
    return rva
# Resolver slots de la vtable CharBody 0x16F8A68
print("=== Vtable CharBody 0x16F8A68 RTTI:", rtti_from_vt(0x16F8A68), "===")
VT=0x16F8A68
for slot in [0x0,0x8,0x10,0x20,0x58,0x60]:
    p=qword(VT+slot)
    if p:
        r=p-IB; real=resolve_thunk(r)
        print(f"  CharBody::vt+0x{slot:<3X} -> 0x{r:X} (real 0x{real:X})")
# Confirmar: el tick (rama viva 0x5CD26B) llama 0x5C67C0 = mov rcx,[rcx+8]; jmp [CombatClass.vt+0x20]
# Pero la materializacion 0x5C6D20 es CharBody::vt+0x8 = startAction-like. Quien la llama?
# El CombatClass tick (0x60D470) o el Task. Verificar si 0x5C6D20 es llamado desde periodicUpdate->0x5C67C0->CombatClass::tick
# CombatClass::tick 0x60D470 ya lo vimos. Veamos si llama [CharBody.vt+0x8].
print("\n=== quien materializa: el tick ejecuta char+0x448(?)+0xE8->vt+0x10. Confirmar char+0x448 tipo ===")
# char+0x448: leer su ctor 0x63A2F0 — su vtable es indirecta. Mejor: en createComponents call[vtbl+0x418] crea AnimationClass.
# El objeto en char+0x448 cuyo +0xE8 = Tasker activo. Confirmamos via runAction: tick hace mov rcx,[r11+0xE8]; mov rax,[rcx]; call[rax+0x10]
# rax+0x10 = runAction (Tasker vt+0x10). Asi que char+0x448+0xE8 = Tasker* activo (currentAction del AnimationClass).
print("char+0x448 = AnimationClass; AnimationClass+0xE8 = Tasker* activo ejecutado en el tick (vt+0x10=runAction).")
