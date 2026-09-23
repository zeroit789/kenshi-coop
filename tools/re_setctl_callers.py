# -*- coding: utf-8 -*-
# ES: Verifica la firma real de SetControlledChar (0x802520): desensambla su cola (0x80268E, mensaje
#     "Player now controlling") y, para hasta 6 llamadores (call E8), los 0x30 bytes previos marcando
#     cómo se cargan rcx (this) y rdx (argumento). Uso: python re_setctl_callers.py
# EN: Verifies the real signature of SetControlledChar (0x802520): disassembles its tail (0x80268E,
#     "Player now controlling" message) and, for up to 6 callers (E8 call), the previous 0x30 bytes
#     flagging how rcx (this) and rdx (argument) are loaded. Usage: python re_setctl_callers.py

# Verifica la FIRMA real de SetControlledChar 0x802520: que pasan los callers en rcx (this) y
# rdx (arg). Y desensambla el resto de SetControlledChar (0x80268E en adelante) para ver el
# 'Player now controlling' y como deriva el char de la faction. Tambien dump del virtual +0x58
# para confirmar que devuelve char+0x10 (=faction). Decide la semantica del gate +0x250.
# EN: Verifies the REAL signature of SetControlledChar 0x802520: what callers pass in rcx (this) and
#     rdx (arg). And disassembles the rest of SetControlledChar (0x80268E onwards) to see the
#     'Player now controlling' and how it derives the char from the faction. Also a dump of virtual +0x58
#     to confirm it returns char+0x10 (=faction) (not implemented here). Decides the semantics of gate +0x250.
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
# ES: Primera sección (se asume .text).
# EN: First section (assumed to be .text).
TEXT=SEC[0]
# ES: RVA -> offset en el fichero (None si el RVA no tiene datos en disco).
# EN: RVA -> file offset (None if the RVA has no on-disk data).
def r2o(rva):
    for srva,vs,ro,rs in SEC:
        if srva<=rva<srva+max(vs,rs) and rva-srva<rs: return ro+(rva-srva)
# ES: Offset de fichero -> RVA.
# EN: File offset -> RVA.
def o2r(off):
    for srva,vs,ro,rs in SEC:
        if ro<=off<ro+rs: return srva+(off-ro)
# ES: Desensambla n bytes desde rva; para en int3.
# EN: Disassembles n bytes from rva; stops at int3.
def dump(rva,n,label):
    o=r2o(rva); code=DATA[o:o+n]; dec=Decoder(64,code,ip=IMAGE_BASE+rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} 0x{rva:X} ===")
    for ins in dec:
        rv=ins.ip-IMAGE_BASE; raw=code[ins.ip-(IMAGE_BASE+rva):ins.ip-(IMAGE_BASE+rva)+ins.len]
        print(f"  0x{rv:08X}  {' '.join(f'{b:02x}' for b in raw):<22} {fmt.format(ins)}")
        if ins.mnemonic==Mnemonic.INT3: break

# resto de SetControlledChar (donde imprime 'Player now controlling' y deriva char)
# EN: rest of SetControlledChar (where it prints 'Player now controlling' and derives the char)
dump(0x80268E, 0x80, "SetControlledChar cola (0x80268E..)")

# callers: buscar 'call 0x802520' = e8 rel32 con destino 0x802520
# EN: callers: look for 'call 0x802520' = e8 rel32 targeting 0x802520
tgt=0x802520
print("\n=== CALLERS de SetControlledChar 0x802520 ===")
ro,rs=TEXT[2],TEXT[3]
found=0
for off in range(ro, ro+rs-5):
    if DATA[off]==0xE8:
        rel=struct.unpack_from("<i",DATA,off+1)[0]
        src=o2r(off)
        if src is None: continue
        dst=src+5+rel
        if dst==tgt:
            found+=1
            # desensamblar 0x30 bytes antes del call para ver rcx/rdx
            # EN: disassemble 0x30 bytes before the call to see rcx/rdx
            start=src-0x30
            print(f"\n--- caller @ 0x{src:X} (contexto previo) ---")
            o=r2o(start); code=DATA[o:o+0x35]; dec=Decoder(64,code,ip=IMAGE_BASE+start)
            fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
            for ins in dec:
                rv=ins.ip-IMAGE_BASE
                s=fmt.format(ins); m=""
                if rv==src: m="  <<< CALL SetControlledChar"
                if ins.mnemonic in (Mnemonic.MOV,Mnemonic.LEA) and ("rcx" in s.split(",")[0] or "rdx" in s.split(",")[0] if "," in s else False):
                    m+="  [arg]"
                print(f"    0x{rv:08X}  {s}{m}")
            if found>=6: break
print(f"\nTotal callers: {found}")
