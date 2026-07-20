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
def resolve_thunk(rva,depth=0):
    if depth>4: return rva,""
    o=r2o(rva)
    if o is None: return rva,""
    try: ins=next(iter(Decoder(64,DATA[o:o+16],ip=IB+rva)))
    except StopIteration: return rva,""
    if ins.mnemonic==Mnemonic.JMP and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
        real=ins.near_branch_target-IB; r2,_=resolve_thunk(real,depth+1); return r2,f" (thunk->0x{r2:X})"
    return rva,""
fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
RN={getattr(Register,n):n for n in dir(Register) if isinstance(getattr(Register,n),int) and not n.startswith("_")}
def dis(rva,maxlen,label,watch=(),stop_ret=True):
    print(f"\n{'='*92}\n=== {label}  RVA 0x{rva:X} ===\n{'='*92}")
    o=r2o(rva); code=DATA[o:o+maxlen]
    for ins in Decoder(64,code,ip=IB+rva):
        rr=ins.ip-IB; off=ins.ip-(IB+rva)
        raw=" ".join(f"{b:02x}" for b in code[off:off+ins.len]); note=""
        if ins.mnemonic==Mnemonic.CALL and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
            t=ins.near_branch_target-IB; real,th=resolve_thunk(t); note=f"   ; CALL 0x{t:X}{th}"
        elif ins.mnemonic==Mnemonic.CALL and ins.op0_kind==OpKind.MEMORY:
            note=f"   ; CALL IND [{RN.get(ins.memory_base,'?')}+0x{ins.memory_displacement:X}]"
        if ins.memory_displacement in watch and (ins.op0_kind==OpKind.MEMORY or ins.op1_kind==OpKind.MEMORY):
            tag="WRITE" if ins.op0_kind==OpKind.MEMORY else "read"
            note+=f"   <<<<{tag} +0x{ins.memory_displacement:X}"
        print(f"0x{rr:08X}  {raw:<26} {fmt.format(ins)}{note}")
        if ins.mnemonic==Mnemonic.INT3: print("  <--PAD"); break
        if stop_ret and ins.mnemonic==Mnemonic.RET: break
# startAction de Task_MeleeAttack: ¿escribe AnimationClass+0xE8 (currentAction)?
dis(0x3341F0, 0x120, "Task_MeleeAttack::startAction 0x3341F0", watch=(0xE8,0x68,0x70))
# runAction base Tasker (vt+0x10) = purecall? y la de Task_MeleeAttack
dis(0x34CA40, 0x100, "Task_MeleeAttack::runAction 0x34CA40", watch=(0xE8,0x68,0x70))
