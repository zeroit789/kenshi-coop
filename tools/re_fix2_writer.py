# -*- coding: utf-8 -*-
# ES: Verifica el escritor 0x645B7D (FIX2 alternativo: char+0xDC=1) y el contexto del gate char+0xDC,
#     marcando accesos +0xDC/+0x250/+0x10 (facción), e imprime el prólogo de SetControlledChar (0x802520).
#     Uso: python re_fix2_writer.py
# EN: Verifies writer 0x645B7D (alternative FIX2: char+0xDC=1) and the char+0xDC gate context, flagging
#     +0xDC/+0x250/+0x10 (faction) accesses, and prints the prologue of SetControlledChar (0x802520).
#     Usage: python re_fix2_writer.py

# Verifica el writer 0x645B7D (FIX2 alternativo: char+0xDC=1) y el contexto del gate char+0xDC.
# Tambien confirma que char+0x10 == faction (el +0x58 devuelve [rcx+0x10]).
# EN: Verifies writer 0x645B7D (alternative FIX2: char+0xDC=1) and the context of the char+0xDC gate.
#     Also confirms that char+0x10 == faction (+0x58 returns [rcx+0x10]).
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic
EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
DATA = open(EXE,"rb").read()
# ES: Tabla de secciones leída a mano de las cabeceras PE (e_lfanew -> COFF -> cabeceras de sección de 40 bytes).
# EN: Section table parsed by hand from the PE headers (e_lfanew -> COFF -> 40-byte section headers).
e=struct.unpack_from("<I",DATA,0x3C)[0]; coff=e+4
ns=struct.unpack_from("<H",DATA,coff+2)[0]; osz=struct.unpack_from("<H",DATA,coff+16)[0]
opt=coff+20; so=opt+osz; SEC=[]
for i in range(ns):
    o=so+i*40; SEC.append((struct.unpack_from("<I",DATA,o+12)[0],struct.unpack_from("<I",DATA,o+8)[0],
                           struct.unpack_from("<I",DATA,o+20)[0],struct.unpack_from("<I",DATA,o+16)[0]))
# ES: RVA -> offset en el fichero (None si el RVA no tiene datos en disco).
# EN: RVA -> file offset (None if the RVA has no on-disk data).
def r2o(rva):
    for srva,vs,ro,rs in SEC:
        if srva<=rva<srva+max(vs,rs) and rva-srva<rs: return ro+(rva-srva)
# ES: Desensambla n bytes desde rva marcando los campos de interés; para en int3.
# EN: Disassembles n bytes from rva flagging the fields of interest; stops at int3.
def dump(rva,n,label):
    o=r2o(rva); code=DATA[o:o+n]; dec=Decoder(64,code,ip=IMAGE_BASE+rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} 0x{rva:X} ===")
    for ins in dec:
        rv=ins.ip-IMAGE_BASE; raw=code[ins.ip-(IMAGE_BASE+rva):ins.ip-(IMAGE_BASE+rva)+ins.len]
        s=fmt.format(ins); m=""
        if "0xDC" in s: m="  <<+0xDC"
        if "0x250" in s: m+="  <<+0x250"
        if "0x10]" in s: m+="  <<+0x10(faction)"
        print(f"  0x{rv:08X}  {' '.join(f'{b:02x}' for b in raw):<22} {s}{m}")
        if ins.mnemonic==Mnemonic.INT3: break

# ES: Contexto del escritor y prólogo de SetControlledChar.
# EN: Writer context and SetControlledChar prologue.
dump(0x645B60, 0x40, "writer FIX2 char+0xDC (contexto 0x645B7D)")
print("\nPROLOGO SetControlledChar 0x802520:", " ".join(f"{b:02x}" for b in DATA[r2o(0x802520):r2o(0x802520)+8]),
      "  (40 57 48 83 EC 60 = push rdi; sub rsp,0x60 -> NO es mov rax,rsp)")
