# -*- coding: utf-8 -*-
# Buscar callers de 0x5C6D20 (la func de materializacion) en .text
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
text=[s for s in SEC if s[0]==".text"][0]; nm,sr,vs,ro,rs=text
code=DATA[ro:ro+rs]
TARGET=0x5C6D20
# Buscar tambien el thunk jmp 0x5C6D20
thunks=set([TARGET])
for ins in Decoder(64,code,ip=IB+sr):
    if ins.mnemonic==Mnemonic.JMP and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
        if ins.near_branch_target-IB==TARGET:
            thunks.add(ins.ip-IB)
print(f"Thunks/target: {[hex(t) for t in thunks]}")
# Buscar slots de vtable que apunten a TARGET o a su thunk
print("\n-- Slots de vtable (.rdata) que apuntan a la func o su thunk --")
rdata=[s for s in SEC if s[0]==".rdata"][0]
for base in range(rdata[1], rdata[1]+rdata[2], 8):
    q=qword(base)
    if q and (q-IB) in thunks:
        print(f"  vtable slot @RVA 0x{base:X} -> 0x{q-IB:X}")
# callers directos (call rel32)
print("\n-- Callers directos (call) --")
cnt=0
for ins in Decoder(64,code,ip=IB+sr):
    if ins.mnemonic==Mnemonic.CALL and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
        if (ins.near_branch_target-IB) in thunks:
            print(f"  0x{ins.ip-IB:08X} call -> 0x{ins.near_branch_target-IB:X}")
            cnt+=1
            if cnt>20: break
