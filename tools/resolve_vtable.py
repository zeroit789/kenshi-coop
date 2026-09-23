# -*- coding: utf-8 -*-
# ES: Resuelve la vtable de Character vía RTTI (".?AVCharacter@@" -> TypeDescriptor -> COL -> vtable) y lee
#     los slots usados en la rama viva (+0x40, +0x58, +0x60, +0xE0, +0xE8, +0x1D8, +0x1E0, +0x268, +0x270),
#     resolviendo thunks. Uso: python resolve_vtable.py
# EN: Resolves the Character vtable through RTTI (".?AVCharacter@@" -> TypeDescriptor -> COL -> vtable) and
#     reads the slots used in the live branch (+0x40, +0x58, +0x60, +0xE0, +0xE8, +0x1D8, +0x1E0, +0x268,
#     +0x270), resolving thunks. Usage: python resolve_vtable.py

# Resuelve la vtable del Character (via RTTI .?AVCharacter@@) y lee los slots usados en la rama viva.
# Tambien intenta identificar el sub-objeto char+0x448 leyendo su vtable si es localizable estaticamente.
# EN: Resolves the Character vtable (via RTTI .?AVCharacter@@) and reads the slots used in the live branch.
#     Also tries to identify the char+0x448 subobject by reading its vtable if statically locatable
#     (not implemented).
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
with open(EXE, "rb") as f:
    DATA = f.read()
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
        if raw_off <= off < raw_off + raw_size: return srva + (off - raw_off)
    return None
# ES: Lee un qword en un RVA (None si se sale).
# EN: Reads a qword at an RVA (None if out of range).
def qword_at_rva(rva):
    o = rva_to_off(rva)
    if o is None or o+8>len(DATA): return None
    return struct.unpack_from("<Q", DATA, o)[0]
# ES: Offsets de fichero de todas las apariciones de b.
# EN: File offsets of every occurrence of b.
def find_bytes(b):
    res=[]; i=0
    while True:
        p=DATA.find(b,i)
        if p<0: break
        res.append(p); i=p+1
    return res
# ES: Si en la dirección hay un JMP (thunk), devuelve su destino real (según la variante sigue uno o varios saltos).
# EN: If there is a JMP (thunk) at the address, returns its real target (one or several hops depending on the variant).
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
# EN: 1) Locate the .?AVCharacter@@ TypeDescriptor
td_str = b".?AVCharacter@@\x00"
td_hits = find_bytes(td_str)
print(f"TypeDescriptor '.?AVCharacter@@' string hits: {[hex(off_to_rva(h)) for h in td_hits]}")

# El TypeDescriptor empieza 0x10 antes del string (vftable ptr + spare). Buscamos COL que apunte al TD,
# pero es mas robusto: buscar la vtable cuyo COL (en slot-8) -> TypeDescriptor de Character.
# Enfoque directo: para cada hit, el TD_VA = base + (off_string_rva - 0x10). Buscar punteros a TD_VA en .rdata (esos son COLs).
# EN: The TypeDescriptor starts 0x10 before the string (vftable ptr + spare). We look for a COL pointing to the TD;
#     more robust: find the vtable whose COL (at slot-8) -> Character TypeDescriptor.
#     Direct approach: for each hit, TD_VA = base + (off_string_rva - 0x10). Look for pointers to TD_VA in .rdata (those are COLs).
for h in td_hits:
    str_rva = off_to_rva(h)
    td_rva = str_rva - 0x10
    td_va = IMAGE_BASE + td_rva
    # buscar referencias a td_va (en _RTTICompleteObjectLocator, campo pTypeDescriptor es disp32 RVA en x64!)
    # En x64 RTTI usa RVAs (32-bit) relativos a imagebase, no VAs. Campo TypeDescriptor en COL+0xC = td_rva.
    # EN: look for references to td_va (in _RTTICompleteObjectLocator the pTypeDescriptor field is a disp32 RVA on x64!)
    #     On x64 RTTI uses RVAs (32-bit) relative to the image base, not VAs. TypeDescriptor field at COL+0xC = td_rva.
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
        # ES: COL: firma(0) a -0xC del campo TD. El campo TD está en COL+0xC.
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
    # EN: For each COL, the vtable is where a pointer (VA) = base+COL_rva is followed by functions.
    for col_rva in col_candidates:
        col_va = IMAGE_BASE + col_rva
        ndl = struct.pack("<Q", col_va)
        for p in find_bytes(ndl):
            prva = off_to_rva(p)
            if prva is None: continue
            # EN: the vtable starts right after the COL pointer
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
