# -*- coding: utf-8 -*-
# Desensambla DESDE el inicio de la funcion que contiene 0x5CD1C0 (rama viva) para entender
# que es 'rdi' (el objeto sobre el que se llama [vtbl+0x58] y se lee +0x250). Buscamos el
# prologo hacia atras y mostramos como se carga rdi. Tambien desensamblamos 0x5CD1C0..0x5CD270.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl
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
    print(f"\n=== {label} 0x{rva:X}..0x{rva+n:X} ===")
    for ins in dec:
        rv=ins.ip-IMAGE_BASE; raw=code[ins.ip-(IMAGE_BASE+rva):ins.ip-(IMAGE_BASE+rva)+ins.len]
        m=""
        if "rdi" in fmt.format(ins): m="  <<rdi"
        if "0x250" in fmt.format(ins): m+="  <<+0x250"
        if "0x58]" in fmt.format(ins): m+="  <<+0x58"
        print(f"  0x{rv:08X}  {' '.join(f'{b:02x}' for b in raw):<22} {fmt.format(ins)}{m}")

# Buscar el prologo de la funcion: retroceder desde 0x5CD1C0 hasta hallar el inicio.
# El AI tick segun RE es 0x5CCD90. Desensamblamos desde ahi para ver como se establece rdi.
dump(0x5CCD90, 0x60, "Inicio AI tick (0x5CCD90) - como se carga rdi")
dump(0x5CD1A0, 0xD0, "Rama viva contexto (0x5CD1A0)")
