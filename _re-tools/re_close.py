# -*- coding: utf-8 -*-
# ES: Cierre de la investigación de "materialización": resuelve RTTI y slots (0x0-0x60) de la vtable
#     CharBody 0x16F8A68 siguiendo thunks, y deja anotada la conclusión: char+0x448 = AnimationClass y
#     AnimationClass+0xE8 = Tasker* activo que el tick ejecuta (vt+0x10 = runAction).
#     Uso: python re_close.py
# EN: Wrap-up of the "materialization" investigation: resolves RTTI and slots (0x0-0x60) of the CharBody
#     vtable 0x16F8A68 following thunks, and records the conclusion: char+0x448 = AnimationClass and
#     AnimationClass+0xE8 = active Tasker* run by the tick (vt+0x10 = runAction).
#     Usage: python re_close.py

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
# ES: Nombre RTTI de una vtable: vt-8 -> CompleteObjectLocator, COL+0xC -> TypeDescriptor, nombre en TD+0x10.
# EN: RTTI name of a vtable: vt-8 -> CompleteObjectLocator, COL+0xC -> TypeDescriptor, name at TD+0x10.
def rtti_from_vt(vt_rva):
    col=qword(vt_rva-8)
    if not col: return None
    o=r2o(col-IB)
    if o is None: return None
    td=struct.unpack_from("<I",DATA,o+0xC)[0]; to=r2o(td)
    if to is None: return None
    return DATA[to+0x10:to+0x10+80].split(b"\x00")[0].decode("ascii","ignore")
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
# Resolver slots de la vtable CharBody 0x16F8A68
# EN: Resolve the slots of the CharBody vtable 0x16F8A68
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
# EN: Confirm: the tick (live branch 0x5CD26B) calls 0x5C67C0 = mov rcx,[rcx+8]; jmp [CombatClass.vt+0x20]
#     But materialization 0x5C6D20 is CharBody::vt+0x8 = startAction-like. Who calls it?
#     The CombatClass tick (0x60D470) or the Task. Check whether 0x5C6D20 is called from periodicUpdate->0x5C67C0->CombatClass::tick
#     CombatClass::tick 0x60D470 was already inspected. Let us see whether it calls [CharBody.vt+0x8].
print("\n=== quien materializa: el tick ejecuta char+0x448(?)+0xE8->vt+0x10. Confirmar char+0x448 tipo ===")
# char+0x448: leer su ctor 0x63A2F0 — su vtable es indirecta. Mejor: en createComponents call[vtbl+0x418] crea AnimationClass.
# El objeto en char+0x448 cuyo +0xE8 = Tasker activo. Confirmamos via runAction: tick hace mov rcx,[r11+0xE8]; mov rax,[rcx]; call[rax+0x10]
# rax+0x10 = runAction (Tasker vt+0x10). Asi que char+0x448+0xE8 = Tasker* activo (currentAction del AnimationClass).
# EN: char+0x448: read its ctor 0x63A2F0 - its vtable is indirect. Better: in createComponents call[vtbl+0x418] creates AnimationClass.
#     The object at char+0x448 whose +0xE8 = active Tasker. Confirmed through runAction: tick does mov rcx,[r11+0xE8]; mov rax,[rcx]; call[rax+0x10]
#     rax+0x10 = runAction (Tasker vt+0x10). So char+0x448+0xE8 = active Tasker* (currentAction of AnimationClass).
print("char+0x448 = AnimationClass; AnimationClass+0xE8 = Tasker* activo ejecutado en el tick (vt+0x10=runAction).")
