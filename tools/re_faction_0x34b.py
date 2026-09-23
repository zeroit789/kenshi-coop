# -*- coding: utf-8 -*-
# ES: Segunda verificación de Faction+0x34 con RVAs de 1.0.68 dados por otra investigación (ctor 0x802070,
#     getOrCreateFaction 0x2E77F0, arranque Nameless 0x86DB80) y escaneo de .text de getters triviales
#     "mov eax,[rcx+0x34]; ret" (8B 41 34 C3). Uso: python re_faction_0x34b.py
# EN: Second verification of Faction+0x34 using 1.0.68 RVAs given by another investigation (ctor 0x802070,
#     getOrCreateFaction 0x2E77F0, Nameless bootstrap 0x86DB80) and a .text scan for trivial getters
#     "mov eax,[rcx+0x34]; ret" (8B 41 34 C3). Usage: python re_faction_0x34b.py

# Verificacion Faction+0x34 usando RVAs reales de 1.0.68 que cito el otro agente:
# ctor 0x802070, getOrCreateFaction 0x2E77F0, bootstrap Nameless 0x86DB80.
# Tambien escaneo .text en busca de getters triviales 'mov eax,[rXX+0x34]; ret'.
# EN: Faction+0x34 verification using the real 1.0.68 RVAs cited by the other investigation:
#     ctor 0x802070, getOrCreateFaction 0x2E77F0, Nameless bootstrap 0x86DB80.
#     Also a .text scan for trivial getters 'mov eax,[rXX+0x34]; ret'.
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
            if d < raw_size:
                return raw_off + d
    return None

fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
# ES: Desensambla un rango desde start_rva e imprime cada instrucción con anotaciones (destinos, cadenas o marcas según el script).
# EN: Disassembles a range from start_rva and prints each instruction with annotations (targets, strings or marks depending on the script).
def disasm(start_rva, length, label):
    off = rva_to_off(start_rva)
    if off is None:
        print(f"\n=== {label} 0x{start_rva:X}: RVA fuera de seccion ==="); return
    code = DATA[off:off+length]
    dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    print(f"\n=== {label} desde RVA 0x{start_rva:X} ===")
    base = IMAGE_BASE+start_rva
    for instr in dec:
        rva = instr.ip - IMAGE_BASE
        raw = code[instr.ip-base:instr.ip-base+instr.len]
        rawhex = " ".join(f"{b:02x}" for b in raw)
        text = fmt.format(instr)
        mark = "   <== +0x34" if "+0x34]" in text else ""
        print(f"0x{rva:08X}  {rawhex:<28} {text}{mark}")
        if instr.mnemonic == Mnemonic.INT3:
            print("  (padding)"); break

disasm(0x802070, 0x260, "ctor candidato (otro agente)")
disasm(0x2E77F0, 0x120, "getOrCreateFaction candidato (otro agente)")
disasm(0x86DB80, 0x200, "bootstrap Nameless candidato (otro agente)")

# Escaneo .text: getters triviales que leen +0x34 y retornan.
# Patron 1: 8B 41 34 C3            mov eax,[rcx+0x34]; ret
# Patron 2: 8B 42 34 C3            mov eax,[rdx+0x34]; ret
# Patron 3: 48 8B 01 8B 40 34 C3   (indireccion) -- menos probable
# EN: .text scan: trivial getters reading +0x34 and returning.
#     Pattern 1: 8B 41 34 C3            mov eax,[rcx+0x34]; ret
#     Pattern 2: 8B 42 34 C3            mov eax,[rdx+0x34]; ret
#     Pattern 3: 48 8B 01 8B 40 34 C3   (indirection) -- less likely
#     (only pattern 1 is searched)
print("\n=== Escaneo .text: 'mov eax,[rcx+0x34]; ret' (8B 41 34 C3) ===")
needle = bytes([0x8B,0x41,0x34,0xC3])
for name, srva, vsize, raw_off, raw_size in SECTIONS:
    if name != ".text": continue
    seg = DATA[raw_off:raw_off+raw_size]
    idx = 0; hits=0
    while True:
        j = seg.find(needle, idx)
        if j < 0: break
        # filtrar: la instruccion previa debe ser inicio de funcion (cc/ret antes o alineacion)
        # EN: filter: the previous instruction should be a function start (cc/ret before or alignment); only printed, not filtered
        prev = seg[j-1] if j>0 else 0
        rva = srva + j
        print(f"  hit @ RVA 0x{rva:08X}  prevbyte=0x{prev:02X}")
        hits+=1; idx = j+1
        if hits>40: print("  ...mas"); break
