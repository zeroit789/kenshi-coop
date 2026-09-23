# -*- coding: utf-8 -*-
# ES: Localiza la vtable de Character vía RTTI (TypeDescriptor ".?AVCharacter@@" -> COL -> vtable),
#     imprime varios slots clave y desensambla getController (slot +0x58). Uso: python re_charvtbl.py
# EN: Finds the Character vtable through RTTI (".?AVCharacter@@" TypeDescriptor -> COL -> vtable),
#     prints several key slots and disassembles getController (slot +0x58). Usage: python re_charvtbl.py

# Localiza la vtable de Character via RTTI COL y desensambla getController (slot +0x58).
# EN: Finds the Character vtable through the RTTI COL and disassembles getController (slot +0x58).
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic

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
        if raw_off <= off < raw_off+raw_size:
            return srva + (off - raw_off)
    return None
# ES: Lee un qword (8 bytes) en un RVA.
# EN: Reads a qword (8 bytes) at an RVA.
def read_q(rva):
    off = rva_to_off(rva); return struct.unpack_from("<Q", DATA, off)[0]
# ES: Lee un dword (4 bytes) en un RVA.
# EN: Reads a dword (4 bytes) at an RVA.
def read_d(rva):
    off = rva_to_off(rva); return struct.unpack_from("<I", DATA, off)[0]

# TypeDescriptor de Character
# EN: Character TypeDescriptor
needle = b".?AVCharacter@@"
idx = DATA.find(needle)
td_rva = off_to_rva(idx - 0x10)   # TD = 0x10 antes del nombre (vftptr@+0, spare@+8, name@+0x10)
# EN: TD = 0x10 before the name (vftptr@+0, spare@+8, name@+0x10)
print("TypeDescriptor Character RVA:", hex(td_rva))

# Buscar COLs (CompleteObjectLocator) que referencien este TD.
# COL layout (x64): +0x00 signature, +0x04 offset, +0x08 cdOffset, +0x0C pTypeDescriptor(RVA),
#                   +0x10 pClassDescriptor(RVA), +0x14 pSelf(RVA).
# pTypeDescriptor es un RVA (32-bit) == td_rva.
# EN: Find COLs (CompleteObjectLocator) referencing this TD.
#     COL layout (x64): +0x00 signature, +0x04 offset, +0x08 cdOffset, +0x0C pTypeDescriptor(RVA),
#                       +0x10 pClassDescriptor(RVA), +0x14 pSelf(RVA).
#     pTypeDescriptor is an RVA (32-bit) == td_rva.
rd = next(s for s in SECTIONS if s[0]==".rdata")
rd_off, rd_size, rd_rva = rd[3], rd[4], rd[1]
cols = []
for off in range(rd_off, rd_off+rd_size-4, 4):
    if struct.unpack_from("<I", DATA, off)[0] == td_rva:
        col_rva = off_to_rva(off) - 0x0C   # este campo es +0x0C del COL
        # EN: this field is +0x0C of the COL
        # validar signature (0 o 1) y self-ref
        # EN: validate signature (0 or 1) and self-ref (only the signature is checked)
        try:
            sig = read_d(col_rva)
            if sig in (0,1):
                cols.append(col_rva)
        except Exception:
            pass
print("COLs candidatos:", [hex(c) for c in cols])

# La vtable: justo despues del COL-ptr en .rdata. vtable[-1] (qword en vtbl-8) = VA del COL.
# EN: The vtable: right after the COL pointer in .rdata. vtable[-1] (qword at vtbl-8) = COL VA.
for col in cols:
    col_va = IMAGE_BASE + col
    # buscar en .rdata un qword == col_va; vtbl_base = ese_rva + 8
    # EN: find in .rdata a qword == col_va; vtbl_base = that rva + 8
    for off in range(rd_off, rd_off+rd_size-8, 8):
        if struct.unpack_from("<Q", DATA, off)[0] == col_va:
            vtbl_rva = off_to_rva(off) + 8
            print(f"\n=== vtable de Character @ RVA 0x{vtbl_rva:X} (COL 0x{col:X}) ===")
            # dump slots clave
            # EN: dump key slots
            for so in (0x58, 0xE8, 0xE0, 0x1D8, 0x1E0, 0x268, 0x270):
                fn = read_q(vtbl_rva + so) - IMAGE_BASE
                print(f"  slot +0x{so:<4X} -> 0x{fn:X}")
            getctl = read_q(vtbl_rva + 0x58) - IMAGE_BASE
            print(f"\n  -- getController (slot +0x58) = 0x{getctl:X} --")
            o = rva_to_off(getctl)
            code = DATA[o:o+0x30]
            dec = Decoder(64, code, ip=IMAGE_BASE+getctl)
            fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
            for instr in dec:
                rva = instr.ip - IMAGE_BASE
                raw = code[instr.ip-(IMAGE_BASE+getctl):instr.ip-(IMAGE_BASE+getctl)+instr.len]
                print(f"    0x{rva:08X}  {' '.join(f'{b:02x}' for b in raw):<18} {fmt.format(instr)}")
                if instr.mnemonic in (Mnemonic.RET, Mnemonic.INT3): break
            break
