# -*- coding: utf-8 -*-
# Inspecciona prologos de funciones clave y busca strings ancla cercanos para identificar su rol.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
with open(EXE,"rb") as f: DATA=f.read()
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
def o2r(o):
    for nm,sr,vs,ro,rs in SEC:
        if ro<=o<ro+rs: return sr+(o-ro)
    return None

fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""

def dump(rva, n, label):
    print(f"\n=== {label}  RVA 0x{rva:X} ===")
    o=r2o(rva); code=DATA[o:o+n]
    dec=Decoder(64,code,ip=IMAGE_BASE+rva)
    cnt=0
    for ins in dec:
        rr=ins.ip-IMAGE_BASE
        # detectar lea rip-relativo a string
        s=""
        if ins.mnemonic in (Mnemonic.LEA,Mnemonic.MOV) and ins.op1_kind==OpKind.MEMORY and ins.memory_base==0:
            tgt=ins.memory_displacement
            to=r2o(tgt-IMAGE_BASE) if tgt>IMAGE_BASE else None
            if to and 0<=to<len(DATA):
                raw=DATA[to:to+40]
                if all(32<=b<127 or b==0 for b in raw[:8]) and raw[0]!=0:
                    txt=raw.split(b"\x00")[0].decode("ascii","ignore")
                    if len(txt)>=3: s=f"   ; \"{txt}\""
        print(f"0x{rr:08X}  {fmt.format(ins)}{s}")
        cnt+=1
        if cnt>n or ins.mnemonic==Mnemonic.INT3: break

dump(0x66CB50, 40, "recalc reloj por-char (0x5CD1C0 call)")
dump(0xA0AF10, 60, "commit accion a GameWorld (0x5CD254 call)")
dump(0x5C67C0, 40, "func de char+0x648 (0x5CD26B call)")
dump(0x5CE020, 30, "vtbl+0x1D8 = THINK pesado")
dump(0x5E1E60, 30, "vtbl+0x1E0 = segundo think (bool)")
