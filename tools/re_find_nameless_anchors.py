# -*- coding: utf-8 -*-
# ES: Anclas por cadenas: localiza "204-gamedata.base", "Nameless" y nombres de funciones de
#     FactionManager en el binario, y lista los xrefs RIP-relativos a un RVA con el inicio estimado de
#     su función, para relocalizar funciones en Steam 1.0.68.
#     Uso: python re_find_nameless_anchors.py strings | xref <rva_hex>
# EN: String anchors: finds "204-gamedata.base", "Nameless" and FactionManager function names in the
#     binary, and lists RIP-relative xrefs to an RVA with the estimated start of their function, to
#     relocate functions in Steam 1.0.68.
#     Usage: python re_find_nameless_anchors.py strings | xref <rva_hex>

# Ancla por strings: localiza "204-gamedata.base", strings de FactionManager/StringID,
# y sus xrefs RIP-relativos en .text para relocalizar funciones en Steam 1.0.68.
# EN: String anchor: finds "204-gamedata.base", FactionManager/StringID strings,
#     and their RIP-relative xrefs in .text to relocate functions in Steam 1.0.68.
import struct, sys
from iced_x86 import (Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl)

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
DATA = open(EXE, "rb").read()
# ES: Tabla de secciones leída a mano de las cabeceras PE (e_lfanew -> COFF -> cabeceras de sección de 40 bytes).
# EN: Section table parsed by hand from the PE headers (e_lfanew -> COFF -> 40-byte section headers).
e_lfanew = struct.unpack_from("<I", DATA, 0x3C)[0]
coff = e_lfanew + 4
num_sec = struct.unpack_from("<H", DATA, coff + 2)[0]
opt_size = struct.unpack_from("<H", DATA, coff + 16)[0]
opt = coff + 20
sec_off = opt + opt_size
SECTIONS = []
for i in range(num_sec):
    o = sec_off + i*40
    name = DATA[o:o+8].rstrip(b"\x00").decode("ascii","ignore")
    vsize = struct.unpack_from("<I", DATA, o+8)[0]
    rva = struct.unpack_from("<I", DATA, o+12)[0]
    raw_size = struct.unpack_from("<I", DATA, o+16)[0]
    raw_off = struct.unpack_from("<I", DATA, o+20)[0]
    SECTIONS.append((name, rva, vsize, raw_off, raw_size))
TEXT = next(s for s in SECTIONS if s[0]==".text")

# ES: RVA -> offset en el fichero (None si el RVA no tiene datos en disco).
# EN: RVA -> file offset (None if the RVA has no on-disk data).
def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size: return raw_off + d
    return None
# ES: Offset de fichero -> RVA.
# EN: File offset -> RVA.
def off_to_rva(off):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if raw_off <= off < raw_off + raw_size:
            return off - raw_off + srva
    return None
# ES: Nombre de la sección que contiene el RVA.
# EN: Name of the section containing the RVA.
def sec_of(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size): return name
    return None

# ES: Offsets de fichero de todas las apariciones de b.
# EN: File offsets of every occurrence of b.
def find_all(b, start=0, end=None):
    res=[]; i=start
    end = end if end else len(DATA)
    while True:
        p = DATA.find(b, i, end)
        if p==-1: break
        res.append(p); i=p+1
    return res

# ES: Instrucciones RIP-relativas (lea/mov/store) de .text que apuntan a target_rva.
# EN: RIP-relative instructions (lea/mov/store) in .text pointing to target_rva.
def xrefs_to_rva(target_rva):
    """RIP-relativos (lea/mov) en .text que apuntan a target_rva."""
    t0=TEXT[3]; tsz=TEXT[4]
    out=[]
    d=DATA
    for i in range(t0, t0+tsz-7):
        b0=d[i]; b1=d[i+1]
        if b0 in (0x48,0x4C) and b1 in (0x8D,0x8B,0x89):
            modrm=d[i+2]; mod=(modrm>>6)&3; rm=modrm&7
            if mod==0 and rm==5:
                disp=struct.unpack_from("<i",d,i+3)[0]
                ir=off_to_rva(i)
                if ir is None: continue
                if ir+7+disp==target_rva:
                    out.append((ir,{0x8D:"lea",0x8B:"mov",0x89:"movst"}[b1]))
    return out

# ES: Estima el inicio de la función que contiene target retrocediendo hasta el relleno CC CC.
# EN: Estimates the start of the function containing target by walking back to the CC CC padding.
# ES: Inicio estimado de la función: primer byte tras un CC hacia atrás.
# EN: Estimated function start: first byte after a CC walking backwards.
def func_start(rva, back=0x600):
    """Busca hacia atras el inicio de funcion (tras int3/alineamiento)."""
    off=rva_to_off(rva)
    for k in range(1, back):
        # patron prologo comun o tras CC CC
        # EN: common prologue pattern or after CC CC
        if DATA[off-k]==0xCC and DATA[off-k+1]!=0xCC:
            return off_to_rva(off-k+1)
    return None

# ES: Cadenas ancla a buscar.
# EN: Anchor strings to search for.
STRINGS = [
    b"204-gamedata.base",
    b"Nameless",
    b"getFactionByStringID",
    b"getFactionByName",
    b"FactionManager",
    b"getOrCreateFaction",
    b"setupAndLinkAllFactions",
]

# ES: Punto de entrada: modo strings o xref.
# EN: Entry point: strings or xref mode.
if __name__=="__main__":
    mode = sys.argv[1] if len(sys.argv)>1 else "strings"
    if mode=="strings":
        for s in STRINGS:
            hits = find_all(s)
            print(f'\n"{s.decode()}" -> {len(hits)} hits')
            for p in hits[:12]:
                rv = off_to_rva(p)
                rvs = f"0x{rv:X}" if rv else "?"
                ctx = DATA[max(0,p-1):p+len(s)+1]
                print(f"  raw 0x{p:X}  RVA {rvs}  sec={sec_of(rv) if rv else '?'}  ctx={ctx}")
    elif mode=="xref":
        target=int(sys.argv[2],16)
        xs=xrefs_to_rva(target)
        print(f"xrefs a 0x{target:X}: {len(xs)}")
        for ir,t in xs[:40]:
            fs=func_start(ir)
            fss = f"0x{fs:X}" if fs else "?"
            print(f"  ref@0x{ir:X} ({t})  func_start~{fss}")
