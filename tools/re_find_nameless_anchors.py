# -*- coding: utf-8 -*-
# Ancla por strings: localiza "204-gamedata.base", strings de FactionManager/StringID,
# y sus xrefs RIP-relativos en .text para relocalizar funciones en Steam 1.0.68.
import struct, sys
from iced_x86 import (Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl)

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
DATA = open(EXE, "rb").read()
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

def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size: return raw_off + d
    return None
def off_to_rva(off):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if raw_off <= off < raw_off + raw_size:
            return off - raw_off + srva
    return None
def sec_of(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size): return name
    return None

def find_all(b, start=0, end=None):
    res=[]; i=start
    end = end if end else len(DATA)
    while True:
        p = DATA.find(b, i, end)
        if p==-1: break
        res.append(p); i=p+1
    return res

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

def func_start(rva, back=0x600):
    """Busca hacia atras el inicio de funcion (tras int3/alineamiento)."""
    off=rva_to_off(rva)
    for k in range(1, back):
        # patron prologo comun o tras CC CC
        if DATA[off-k]==0xCC and DATA[off-k+1]!=0xCC:
            return off_to_rva(off-k+1)
    return None

STRINGS = [
    b"204-gamedata.base",
    b"Nameless",
    b"getFactionByStringID",
    b"getFactionByName",
    b"FactionManager",
    b"getOrCreateFaction",
    b"setupAndLinkAllFactions",
]

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
