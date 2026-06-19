# -*- coding: utf-8 -*-
# Verifica el writer 0x645B7D (FIX2 alternativo: char+0xDC=1) y el contexto del gate char+0xDC.
# Tambien confirma que char+0x10 == faction (el +0x58 devuelve [rcx+0x10]).
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic
EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
DATA = open(EXE,"rb").read()
e=struct.unpack_from("<I",DATA,0x3C)[0]; coff=e+4
ns=struct.unpack_from("<H",DATA,coff+2)[0]; osz=struct.unpack_from("<H",DATA,coff+16)[0]
opt=coff+20; so=opt+osz; SEC=[]
for i in range(ns):
    o=so+i*40; SEC.append((struct.unpack_from("<I",DATA,o+12)[0],struct.unpack_from("<I",DATA,o+8)[0],
                           struct.unpack_from("<I",DATA,o+20)[0],struct.unpack_from("<I",DATA,o+16)[0]))
def r2o(rva):
    for srva,vs,ro,rs in SEC:
        if srva<=rva<srva+max(vs,rs) and rva-srva<rs: return ro+(rva-srva)
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

dump(0x645B60, 0x40, "writer FIX2 char+0xDC (contexto 0x645B7D)")
print("\nPROLOGO SetControlledChar 0x802520:", " ".join(f"{b:02x}" for b in DATA[r2o(0x802520):r2o(0x802520)+8]),
      "  (40 57 48 83 EC 60 = push rdi; sub rsp,0x60 -> NO es mov rax,rsp)")
