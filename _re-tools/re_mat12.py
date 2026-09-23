# -*- coding: utf-8 -*-
# ES: Recorre el constructor de Character (0x6223F0, 0x600 bytes) mostrando los lea de vtables (con RTTI)
#     y las escrituras a char+0x448/+0x640/+0x648/+0x650/+0x658 junto al último call (ctor) previo.
#     Conclusión anotada: el ctor solo pone a cero; los objetos reales se crean en createComponents.
#     Uso: python re_mat12.py
# EN: Walks the Character constructor (0x6223F0, 0x600 bytes) showing vtable leas (with RTTI) and the
#     writes to char+0x448/+0x640/+0x648/+0x650/+0x658 together with the last preceding call (ctor).
#     Recorded conclusion: the ctor only zeroes them; the real objects are built in createComponents.
#     Usage: python re_mat12.py

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
    td=struct.unpack_from("<I",DATA,o+0xC)[0]
    to=r2o(td)
    if to is None: return None
    return DATA[to+0x10:to+0x10+80].split(b"\x00")[0].decode("ascii","ignore")
# ES: Formateador Intel con prefijo 0x y mapa valor -> nombre de registro (RN).
# EN: Intel formatter with 0x prefix and value -> register name map (RN).
fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
RN={getattr(Register,n):n for n in dir(Register) if isinstance(getattr(Register,n),int) and not n.startswith("_")}
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

# Desensamblar Character::ctor 0x6223F0 buscando los ctors/escrituras de +0x448, +0x648, +0x650, +0x658
# Foco: ver que vtable se instala en el objeto de char+0x448
# EN: Disassemble Character::ctor 0x6223F0 looking for the ctors/writes of +0x448, +0x648, +0x650, +0x658
#     Focus: see which vtable is installed in the char+0x448 object
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
        # EN: record the lea of a possible vtable
        if nm: print(f"  0x{rr:08X}  lea {RN.get(ins.op0_register,'?')}, [0x{tgt:X}]  ; vtable RTTI={nm}")
    if ins.mnemonic==Mnemonic.MOV and ins.op0_kind==OpKind.MEMORY and ins.memory_displacement in (0x448,0x648,0x650,0x658,0x640):
        src = RN.get(ins.op1_register,'imm') if ins.op1_kind==OpKind.REGISTER else 'imm/other'
        print(f"  0x{rr:08X}  mov [char+0x{ins.memory_displacement:X}], {src}   (last ctor call=0x{last_call:X})" if last_call else f"  0x{rr:08X}  mov [char+0x{ins.memory_displacement:X}], {src}")
    if ins.mnemonic==Mnemonic.INT3: break
print("\nNota: en createComponents (no ctor) es donde se alocan los objetos reales. El ctor solo nulifica.")
