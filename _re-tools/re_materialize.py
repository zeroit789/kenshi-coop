# -*- coding: utf-8 -*-
# ES: Desensambla completo periodicUpdate (0x5CCD90, tick de IA de cada personaje: gate isDead + salto a
#     la rama viva 0x5CD1C0) para ver cómo se lee y ejecuta char+0x448 (AnimationClass*)+0xE8, el Task
#     activo. Uso: python re_materialize.py
# EN: Disassembles the whole periodicUpdate (0x5CCD90, per-character AI tick: isDead gate + jump to the
#     live branch 0x5CD1C0) to see how char+0x448 (AnimationClass*)+0xE8, the active Task, is read and
#     run. Usage: python re_materialize.py

# RE materializacion de Task: periodicUpdate 0x5CCD90 + rama viva 0x5CD1C0
# Foco: como se lee/ejecuta char+0x448(AnimationClass*)+0xE8 -> Task activo
# EN: Task materialization RE: periodicUpdate 0x5CCD90 + live branch 0x5CD1C0
#     Focus: how char+0x448(AnimationClass*)+0xE8 -> active Task is read/run
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, FlowControl, Register

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IB = 0x140000000
with open(EXE,"rb") as f: DATA=f.read()
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
    o=r2o(r)
    return struct.unpack_from("<Q",DATA,o)[0] if o is not None else None
# ES: Si en rva hay un JMP (thunk), lo sigue recursivamente (máx. 4 saltos) hasta la función real.
# EN: If there is a JMP (thunk) at rva, follows it recursively (max 4 hops) to the real function.
def resolve_thunk(rva,depth=0):
    if depth>4: return rva,""
    o=r2o(rva)
    if o is None: return rva,""
    dec=Decoder(64,DATA[o:o+16],ip=IB+rva)
    try: ins=next(iter(dec))
    except StopIteration: return rva,""
    if ins.mnemonic==Mnemonic.JMP and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
        real=ins.near_branch_target-IB
        r2,_=resolve_thunk(real,depth+1)
        return r2,f" (thunk->0x{r2:X})"
    return rva,""

# ES: Formateador Intel con prefijo 0x y mapa valor -> nombre de registro (RN).
# EN: Intel formatter with 0x prefix and value -> register name map (RN).
fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
RN={getattr(Register,n):n for n in dir(Register) if isinstance(getattr(Register,n),int) and not n.startswith("_")}

# ES: Desensambla maxlen bytes desde rva anotando destinos de call (con thunks resueltos), calls
#     indirectos y otros datos; se detiene en int3 (relleno) y, si stop_ret, en el primer ret.
# EN: Disassembles maxlen bytes from rva annotating call targets (thunks resolved), indirect
#     calls and other details; stops at int3 (padding) and, if stop_ret, at the first ret.
def dis(rva, maxlen, label, stop_on_ret=True):
    print(f"\n{'='*92}\n=== {label}  RVA 0x{rva:X} ===\n{'='*92}")
    o=r2o(rva); code=DATA[o:o+maxlen]
    dec=Decoder(64,code,ip=IB+rva)
    for ins in dec:
        rr=ins.ip-IB
        off=ins.ip-(IB+rva)
        raw=" ".join(f"{b:02x}" for b in code[off:off+ins.len])
        note=""
        # call directo -> resolver thunk
        # EN: direct call -> resolve thunk
        if ins.mnemonic==Mnemonic.CALL and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
            t=ins.near_branch_target-IB
            real,th=resolve_thunk(t)
            note=f"   ; CALL 0x{t:X}{th}"
        elif ins.mnemonic==Mnemonic.CALL and ins.op0_kind==OpKind.MEMORY:
            note=f"   ; CALL INDIRECT [{RN.get(ins.memory_base,'?')}+0x{ins.memory_displacement:X}]"
        # lea/mov a string
        # EN: lea/mov to a string
        if ins.op1_kind==OpKind.MEMORY and ins.memory_base==0 and ins.mnemonic in (Mnemonic.LEA,Mnemonic.MOV):
            tgt=ins.memory_displacement
            to=r2o(tgt-IB) if tgt>IB else None
            if to and 0<=to<len(DATA):
                rawb=DATA[to:to+48]
                if rawb[0]!=0 and all(32<=b<127 or b==0 for b in rawb[:6]):
                    txt=rawb.split(b"\x00")[0].decode("ascii","ignore")
                    if len(txt)>=3: note+=f'   ; "{txt}"'
        print(f"0x{rr:08X}  {raw:<30} {fmt.format(ins)}{note}")
        if ins.mnemonic==Mnemonic.INT3: print("   <-- PADDING"); break
        if stop_on_ret and ins.mnemonic==Mnemonic.RET: break

# periodicUpdate completo (gate isDead + salto a rama viva)
# EN: full periodicUpdate (isDead gate + jump to the live branch)
dis(0x5CCD90, 0x440, "periodicUpdate (AI tick) 0x5CCD90", stop_on_ret=False)
