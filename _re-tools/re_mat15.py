# -*- coding: utf-8 -*-
# ES: Mira el constructor del objeto de char+0x448 (0x63A2F0): muestra los lea a vtables (con RTTI) y los
#     "mov [reg], reg" que instalan la vtable en this. Conclusión anotada: char+0x448 = AnimationClass
#     y su +0xE8 es el currentAction (Tasker activo). Uso: python re_mat15.py
# EN: Looks at the constructor of the char+0x448 object (0x63A2F0): shows vtable leas (with RTTI) and the
#     "mov [reg], reg" that install the vtable in this. Recorded conclusion: char+0x448 = AnimationClass
#     and its +0xE8 is the currentAction (active Tasker). Usage: python re_mat15.py

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
# ctor de char+0x448 = 0x63A2F0: ver vtable instalada (mov [rcx], lea vtable)
# EN: ctor of char+0x448 = 0x63A2F0: see the installed vtable (mov [rcx], lea vtable)
o=r2o(0x63A2F0); code=DATA[o:o+0x80]
print("=== ctor de char+0x448 (0x63A2F0): vtable que instala ===")
for ins in Decoder(64,code,ip=IB+0x63A2F0):
    rr=ins.ip-IB
    if ins.mnemonic==Mnemonic.LEA and ins.op1_kind==OpKind.MEMORY and ins.memory_base==0:
        tgt=ins.memory_displacement-IB
        nm=rtti_from_vt(tgt) if tgt>0 else None
        if nm: print(f"  0x{rr:08X}  lea -> [0x{tgt:X}]  RTTI={nm}")
    if ins.mnemonic==Mnemonic.MOV and ins.op0_kind==OpKind.MEMORY and ins.memory_displacement==0 and ins.op1_kind==OpKind.REGISTER:
        print(f"  0x{rr:08X}  {fmt.format(ins)}   (instala vtable en [this])")
    if ins.mnemonic==Mnemonic.INT3: break
# Confirmar el tipo real de char+0x448 leyendo su vtable RTTI directo si conocida.
# Verificar: el slot vt+0x10 de char+0x448 (el que se llama como runAction). char+0x448 vtable?
# El ctor instala su vtable; resolvemos el primer lea con RTTI.
# EN: Confirm the real type of char+0x448 by reading its vtable RTTI directly if known.
#     Check: the vt+0x10 slot of char+0x448 (the one called as runAction). char+0x448 vtable?
#     The ctor installs its vtable; we resolve the first lea with RTTI.
print("\n=== Resumen: char+0x448 = AnimationClass (vt) -> su +0xE8 es el currentAction (Tasker activo) ===")
