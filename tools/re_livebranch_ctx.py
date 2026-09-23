# -*- coding: utf-8 -*-
# ES: Desensambla el inicio del tick de IA (0x5CCD90) y el contexto de la rama viva (0x5CD1A0) marcando rdi,
#     +0x250 y +0x58, para entender qué objeto es rdi (sobre el que se llama [vtbl+0x58] y se lee +0x250).
#     Uso: python re_livebranch_ctx.py
# EN: Disassembles the start of the AI tick (0x5CCD90) and the live-branch context (0x5CD1A0) flagging rdi,
#     +0x250 and +0x58, to understand which object rdi is (on which [vtbl+0x58] is called and +0x250 read).
#     Usage: python re_livebranch_ctx.py

# Desensambla DESDE el inicio de la funcion que contiene 0x5CD1C0 (rama viva) para entender
# que es 'rdi' (el objeto sobre el que se llama [vtbl+0x58] y se lee +0x250). Buscamos el
# prologo hacia atras y mostramos como se carga rdi. Tambien desensamblamos 0x5CD1C0..0x5CD270.
# EN: Disassembles FROM the start of the function containing 0x5CD1C0 (live branch) to understand
#     what 'rdi' is (the object on which [vtbl+0x58] is called and +0x250 is read). We look for the
#     prologue backwards and show how rdi is loaded. We also disassemble 0x5CD1C0..0x5CD270.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl
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
# ES: Desensambla n bytes marcando rdi, +0x250 y +0x58.
# EN: Disassembles n bytes flagging rdi, +0x250 and +0x58.
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
# EN: Find the function prologue: walk back from 0x5CD1C0 to the start.
#     Per the RE the AI tick is 0x5CCD90. We disassemble from there to see how rdi is set.
dump(0x5CCD90, 0x60, "Inicio AI tick (0x5CCD90) - como se carga rdi")
dump(0x5CD1A0, 0xD0, "Rama viva contexto (0x5CD1A0)")
