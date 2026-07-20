# -*- coding: utf-8 -*-
# Buscar funciones que escriban CharBody+0x68 con un puntero NO nulo (registro) Y/O CharBody+0x70=0 (materializa)
# y +0x70=1 (idle). Escaneo lineal de TODO .text, agrupando por proximidad.
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
text=[s for s in SEC if s[0]==".text"][0]; nm,sr,vs,ro,rs=text
code=DATA[ro:ro+rs]
fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
RN={getattr(Register,n):n for n in dir(Register) if isinstance(getattr(Register,n),int) and not n.startswith("_")}
# escaneo: registrar instrucciones que escriben [reg+0x70] con imm (0 o 1) y [reg+0x68] con reg (puntero)
hits70_1=[]; hits70_0=[]; hits68p=[]
for ins in Decoder(64,code,ip=IB+sr):
    rr=ins.ip-IB
    if ins.mnemonic==Mnemonic.MOV and ins.op0_kind==OpKind.MEMORY and ins.memory_displacement==0x70 and ins.memory_base!=Register.RSP and ins.memory_base!=Register.RBP:
        if ins.op1_kind in (OpKind.IMMEDIATE8,OpKind.IMMEDIATE8TO32,OpKind.IMMEDIATE8TO64,OpKind.IMMEDIATE32):
            v=ins.immediate(1) if hasattr(ins,'immediate') else None
            try: v=ins.get_immediate(1)
            except: v=ins.immediate8 if hasattr(ins,'immediate8') else '?'
            if v==1: hits70_1.append((rr,RN.get(ins.memory_base,'?')))
            elif v==0: hits70_0.append((rr,RN.get(ins.memory_base,'?')))
    if ins.mnemonic==Mnemonic.MOV and ins.op0_kind==OpKind.MEMORY and ins.memory_displacement==0x68 and ins.op1_kind==OpKind.REGISTER and ins.memory_base!=Register.RSP and ins.memory_base!=Register.RBP:
        hits68p.append((rr,RN.get(ins.memory_base,'?'),RN.get(ins.op1_register,'?')))
print(f"mov byte[reg+0x70],1 (NO rsp/rbp): {len(hits70_1)}")
for rr,b in hits70_1: print(f"  0x{rr:08X} base={b}")
print(f"\nmov [reg+0x68],reg64 (NO rsp/rbp): {len(hits68p)}")
for rr,b,s in hits68p[:60]: print(f"  0x{rr:08X} [{b}+0x68]={s}")
