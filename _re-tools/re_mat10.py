# -*- coding: utf-8 -*-
# ES: Serie re_mat*: investigación de la "materialización" de personajes (cómo el tick ejecuta el Task
#     activo). Este script desensambla 0x5C67C0, el call justo antes de ejecutar el Task en la rama
#     viva (opera sobre char+0x648), marcando accesos +0x68/+0x70 y cadenas. Uso: python re_mat10.py
# EN: re_mat* series: investigation of character "materialization" (how the tick runs the active Task).
#     This script disassembles 0x5C67C0, the call right before running the Task in the live branch
#     (operates on char+0x648), flagging +0x68/+0x70 accesses and strings. Usage: python re_mat10.py

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
# ES: Si en rva hay un JMP (thunk), lo sigue recursivamente (máx. 4 saltos) hasta la función real.
# EN: If there is a JMP (thunk) at rva, follows it recursively (max 4 hops) to the real function.
def resolve_thunk(rva,depth=0):
    if depth>4: return rva,""
    o=r2o(rva)
    if o is None: return rva,""
    try: ins=next(iter(Decoder(64,DATA[o:o+16],ip=IB+rva)))
    except StopIteration: return rva,""
    if ins.mnemonic==Mnemonic.JMP and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
        real=ins.near_branch_target-IB; r2,_=resolve_thunk(real,depth+1); return r2,f" (thunk->0x{r2:X})"
    return rva,""
# ES: Formateador Intel con prefijo 0x y mapa valor -> nombre de registro (RN).
# EN: Intel formatter with 0x prefix and value -> register name map (RN).
fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
RN={getattr(Register,n):n for n in dir(Register) if isinstance(getattr(Register,n),int) and not n.startswith("_")}
# ES: Desensambla maxlen bytes desde rva anotando destinos de call (con thunks resueltos), calls
#     indirectos y otros datos; se detiene en int3 (relleno) y, si stop_ret, en el primer ret.
# EN: Disassembles maxlen bytes from rva annotating call targets (thunks resolved), indirect
#     calls and other details; stops at int3 (padding) and, if stop_ret, at the first ret.
def dis(rva,maxlen,label,stop_ret=True):
    print(f"\n{'='*92}\n=== {label}  RVA 0x{rva:X} ===\n{'='*92}")
    o=r2o(rva); code=DATA[o:o+maxlen]
    for ins in Decoder(64,code,ip=IB+rva):
        rr=ins.ip-IB; off=ins.ip-(IB+rva)
        raw=" ".join(f"{b:02x}" for b in code[off:off+ins.len]); note=""
        if ins.mnemonic==Mnemonic.CALL and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
            t=ins.near_branch_target-IB; real,th=resolve_thunk(t); note=f"   ; CALL 0x{t:X}{th}"
        elif ins.mnemonic==Mnemonic.CALL and ins.op0_kind==OpKind.MEMORY:
            note=f"   ; CALL IND [{RN.get(ins.memory_base,'?')}+0x{ins.memory_displacement:X}]"
        if ins.op0_kind==OpKind.MEMORY and ins.memory_displacement in (0x68,0x70) and ins.mnemonic in (Mnemonic.MOV,Mnemonic.CMP):
            note+=f"   <<< [+0x{ins.memory_displacement:X}]"
        if ins.op1_kind==OpKind.MEMORY and ins.memory_displacement in (0x68,0x70):
            note+=f"   <<< read [+0x{ins.memory_displacement:X}]"
        if ins.op1_kind==OpKind.MEMORY and ins.memory_base==0 and ins.mnemonic in (Mnemonic.LEA,Mnemonic.MOV):
            tgt=ins.memory_displacement; to=r2o(tgt-IB) if tgt>IB else None
            if to and 0<=to<len(DATA):
                rb=DATA[to:to+48]
                if rb[0]!=0 and all(32<=b<127 or b==0 for b in rb[:6]):
                    txt=rb.split(b"\x00")[0].decode("ascii","ignore")
                    if len(txt)>=3: note+=f'   ; "{txt}"'
        print(f"0x{rr:08X}  {raw:<26} {fmt.format(ins)}{note}")
        if ins.mnemonic==Mnemonic.INT3: print("  <--PAD"); break
        if stop_ret and ins.mnemonic==Mnemonic.RET: break
# 0x5C67C0: el call justo antes de ejecutar el Task en la rama viva (opera sobre char+0x648)
# EN: 0x5C67C0: the call right before running the Task in the live branch (operates on char+0x648)
dis(0x5C67C0, 0x140, "func char+0x648 pre-task (0x5CD26B) 0x5C67C0")
