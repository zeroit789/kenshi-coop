# -*- coding: utf-8 -*-
# ES: Lista todas las escrituras "mov [reg+0xE8], reg/imm" de .text (decodificación lineal): candidatas
#     a setter del Task activo (AnimationClass+0xE8). Uso: python re_find_e8.py
# EN: Lists every "mov [reg+0xE8], reg/imm" write in .text (linear decoding): candidates for the active
#     Task setter (AnimationClass+0xE8). Usage: python re_find_e8.py

# Busca escrituras 'mov [reg+0xE8], reg' en .text -> setter del Task activo (AnimationClass+0xE8)
# EN: Finds 'mov [reg+0xE8], reg' writes in .text -> setter of the active Task (AnimationClass+0xE8)
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
text=[s for s in SEC if s[0]==".text"][0]
nm,sr,vs,ro,rs=text
code=DATA[ro:ro+rs]
# ES: Formateador Intel con prefijo 0x y mapa valor -> nombre de registro (RN).
# EN: Intel formatter with 0x prefix and value -> register name map (RN).
fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
RN={getattr(Register,n):n for n in dir(Register) if isinstance(getattr(Register,n),int) and not n.startswith("_")}
# escaneo lineal por bytes: mov [reg+0xE8], reg64 -> opcode 48/4C 89 con modrm disp32 = E8 00 00 00
# Buscamos patrones: 89 ?? E8 00 00 00 con REX.W. Simplificamos desensamblando todo el .text en bloques alineados a func no es viable;
# en su lugar buscamos disp32 == 0x000000E8 en instrucciones mov mem,reg.
# EN: linear byte scan: mov [reg+0xE8], reg64 -> opcode 48/4C 89 with modrm disp32 = E8 00 00 00
#     We look for patterns 89 ?? E8 00 00 00 with REX.W. Disassembling .text in function-aligned blocks is not viable;
#     instead we look for disp32 == 0x000000E8 in mov mem,reg instructions (full linear decode).
res=[]
dec=Decoder(64,code,ip=IB+sr)
for ins in dec:
    if ins.mnemonic==Mnemonic.MOV and ins.op0_kind==OpKind.MEMORY and ins.memory_displacement==0xE8:
        # destino [base+0xE8], fuente reg
        # EN: destination [base+0xE8], source reg
        if ins.op1_kind==OpKind.REGISTER or ins.op1_kind in (OpKind.IMMEDIATE8TO64,OpKind.IMMEDIATE32):
            rr=ins.ip-IB
            res.append((rr, RN.get(ins.memory_base,'?'), fmt.format(ins)))
print(f"Escrituras a [reg+0xE8] en .text: {len(res)}")
for rr,base,txt in res:
    print(f"  0x{rr:08X}  base={base:<5} {txt}")
