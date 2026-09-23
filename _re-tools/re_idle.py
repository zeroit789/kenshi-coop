# -*- coding: utf-8 -*-
# ES: Desensambla la función que contiene 0x5C6E4E (desde su inicio estimado, 0x300 bytes) marcando
#     lecturas/escrituras de los campos +0x68, +0x70 y +0xE8 (estado idle/Task). Uso: python re_idle.py
# EN: Disassembles the function containing 0x5C6E4E (from its estimated start, 0x300 bytes) flagging
#     reads/writes of fields +0x68, +0x70 and +0xE8 (idle/Task state). Usage: python re_idle.py

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
# ES: Estima el inicio de la función que contiene target retrocediendo hasta el relleno CC CC.
# EN: Estimates the start of the function containing target by walking back to the CC CC padding.
def func_start(target):
    o=r2o(target); start=o
    while start>o-0x800:
        if DATA[start-1]==0xCC and DATA[start-2]==0xCC: break
        start-=1
    return target-(o-start), start
# ES: Desensambla la función contenedora de target marcando accesos a los desplazamientos de watch.
# EN: Disassembles target's containing function flagging accesses to the watch displacements.
def dis_func(target,maxlen=0x200,watch=(0x68,0x70,0xE8)):
    srva,o=func_start(target)
    print(f"\n{'='*92}\n=== Funcion contenedora de 0x{target:X} empieza ~0x{srva:X} ===\n{'='*92}")
    code=DATA[o:o+maxlen]
    for ins in Decoder(64,code,ip=IB+srva):
        rr=ins.ip-IB; off=ins.ip-(IB+srva)
        raw=" ".join(f"{b:02x}" for b in code[off:off+ins.len]); note=""
        if ins.mnemonic==Mnemonic.CALL and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
            t=ins.near_branch_target-IB; real,th=resolve_thunk(t); note=f"   ; CALL 0x{t:X}{th}"
        elif ins.mnemonic==Mnemonic.CALL and ins.op0_kind==OpKind.MEMORY:
            note=f"   ; CALL IND [{RN.get(ins.memory_base,'?')}+0x{ins.memory_displacement:X}]"
        if ins.memory_displacement in watch and (ins.op0_kind==OpKind.MEMORY or ins.op1_kind==OpKind.MEMORY) and ins.memory_base not in (Register.RSP,Register.RBP):
            tag="WR" if ins.op0_kind==OpKind.MEMORY else "rd"
            note+=f"   <<<{tag}+0x{ins.memory_displacement:X}"
        print(f"0x{rr:08X}  {raw:<24} {fmt.format(ins)}{note}")
        if ins.mnemonic==Mnemonic.INT3: print("  <--PAD"); break
        if rr>target+0x40 and ins.mnemonic==Mnemonic.RET: break
dis_func(0x5C6E4E, 0x300)
