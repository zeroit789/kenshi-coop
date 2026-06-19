# -*- coding: utf-8 -*-
# Resuelve la vtable del Character (via RTTI .?AVCharacter@@) y lee los slots usados en la rama viva.
# Tambien intenta identificar el sub-objeto char+0x448 leyendo su vtable si es localizable estaticamente.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
with open(EXE, "rb") as f:
    DATA = f.read()
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
def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size: return raw_off + d
    return None
def off_to_rva(off):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if raw_off <= off < raw_off + raw_size: return srva + (off - raw_off)
    return None
def qword_at_rva(rva):
    o = rva_to_off(rva)
    if o is None or o+8>len(DATA): return None
    return struct.unpack_from("<Q", DATA, o)[0]
def find_bytes(b):
    res=[]; i=0
    while True:
        p=DATA.find(b,i)
        if p<0: break
        res.append(p); i=p+1
    return res
def resolve_thunk(rva, depth=0):
    if depth>4: return rva
    o=rva_to_off(rva)
    if o is None: return rva
    dec=Decoder(64,DATA[o:o+16],ip=IMAGE_BASE+rva)
    try: ins=next(iter(dec))
    except StopIteration: return rva
    if ins.mnemonic==Mnemonic.JMP and ins.op0_kind in (OpKind.NEAR_BRANCH64,OpKind.NEAR_BRANCH32):
        return resolve_thunk(ins.near_branch_target-IMAGE_BASE, depth+1)
    return rva

# 1) Localizar TypeDescriptor .?AVCharacter@@
td_str = b".?AVCharacter@@\x00"
td_hits = find_bytes(td_str)
print(f"TypeDescriptor '.?AVCharacter@@' string hits: {[hex(off_to_rva(h)) for h in td_hits]}")

# El TypeDescriptor empieza 0x10 antes del string (vftable ptr + spare). Buscamos COL que apunte al TD,
# pero es mas robusto: buscar la vtable cuyo COL (en slot-8) -> TypeDescriptor de Character.
# Enfoque directo: para cada hit, el TD_VA = base + (off_string_rva - 0x10). Buscar punteros a TD_VA en .rdata (esos son COLs).
for h in td_hits:
    str_rva = off_to_rva(h)
    td_rva = str_rva - 0x10
    td_va = IMAGE_BASE + td_rva
    # buscar referencias a td_va (en _RTTICompleteObjectLocator, campo pTypeDescriptor es disp32 RVA en x64!)
    # En x64 RTTI usa RVAs (32-bit) relativos a imagebase, no VAs. Campo TypeDescriptor en COL+0xC = td_rva.
    needle = struct.pack("<I", td_rva)
    col_candidates = []
    i = 0
    rd = None
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if name == ".rdata":
            rd = (raw_off, raw_size, srva)
    base_off, size, srva = rd
    seg = DATA[base_off:base_off+size]
    pos = 0
    while True:
        p = seg.find(needle, pos)
        if p<0: break
        # COL: signature(0) at -0xC from the TD field. TD field is at COL+0xC.
        col_off = base_off + p - 0xC
        col_rva = off_to_rva(col_off)
        if col_rva is not None:
            sig = struct.unpack_from("<I", DATA, col_off)[0]
            if sig in (0,1):
                col_candidates.append(col_rva)
        pos = p+1
    print(f"TD@0x{td_rva:X}: COL candidates RVA = {[hex(c) for c in col_candidates]}")
    # Para cada COL, la vtable es el lugar donde un puntero (VA) = base+COL_rva esta seguido de funciones.
    for col_rva in col_candidates:
        col_va = IMAGE_BASE + col_rva
        ndl = struct.pack("<Q", col_va)
        for p in find_bytes(ndl):
            prva = off_to_rva(p)
            if prva is None: continue
            vtbl_rva = prva + 8  # vtable empieza justo despues del puntero al COL
            print(f"  -> vtable Character RVA = 0x{vtbl_rva:X} (COL@0x{col_rva:X})")
            SLOTS = [0x40,0x58,0x60,0xE0,0xE8,0x1D8,0x1E0,0x268,0x270]
            for s in SLOTS:
                fn = qword_at_rva(vtbl_rva + s)
                if fn:
                    frva = fn - IMAGE_BASE
                    real = resolve_thunk(frva)
                    extra = f" (real 0x{real:X})" if real!=frva else ""
                    print(f"       slot +0x{s:<4X} -> 0x{frva:X}{extra}")
