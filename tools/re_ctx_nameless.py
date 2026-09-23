# -*- coding: utf-8 -*-
# ES: Desensambla el contexto (0x40 antes, 0x40 después) de cada xref a la cadena "204-gamedata.base"
#     (identificador de la facción Nameless del jugador), anotando cadenas y calls para identificar la
#     función que la usa. Uso: python re_ctx_nameless.py
# EN: Disassembles the context (0x40 before, 0x40 after) of each xref to the "204-gamedata.base" string
#     (identifier of the player's Nameless faction), annotating strings and calls to identify the function
#     using it. Usage: python re_ctx_nameless.py

# Desensambla contexto alrededor de cada xref del literal "204-gamedata.base"
# y resuelve targets de call/lea a strings cercanos para identificar la funcion.
# EN: Disassembles the context around each xref of the "204-gamedata.base" literal
#     and resolves call/lea targets to nearby strings to identify the function.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register, FlowControl

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

# ES: Lee una cadena ASCII terminada en 0 (devuelve None si no parece texto imprimible, según la variante).
# EN: Reads a NUL-terminated ASCII string (returns None if it does not look printable, depending on the variant).
def read_cstr(va, maxlen=64):
    rva = va - IMAGE_BASE
    off = rva_to_off(rva)
    if off is None: return None
    end = DATA.find(b"\x00", off, off+maxlen)
    if end == -1: return None
    try:
        s = DATA[off:end].decode("ascii")
        if all(32 <= ord(c) < 127 for c in s) and len(s) >= 2:
            return s
    except: return None
    return None

# ES: Desensambla un rango desde start_rva e imprime cada instrucción con anotaciones (destinos, cadenas o marcas según el script).
# EN: Disassembles a range from start_rva and prints each instruction with annotations (targets, strings or marks depending on the script).
def disasm(start_rva, length, label):
    off = rva_to_off(start_rva)
    code = DATA[off:off+length]
    dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n========== {label} (RVA 0x{start_rva:X}) ==========")
    for instr in dec:
        rva = instr.ip - IMAGE_BASE
        txt = fmt.format(instr)
        note = ""
        # resolver lea/mov a string
        # EN: resolve lea/mov to a string
        if instr.memory_base == Register.RIP:
            s = read_cstr(instr.memory_displacement)
            if s: note = f'   ; "{s}"'
        # resolver call directo
        # EN: resolve direct call
        if instr.flow_control == FlowControl.CALL and instr.op0_kind == OpKind.NEAR_BRANCH64:
            t = instr.near_branch_target - IMAGE_BASE
            note = f"   ; -> sub_0x{t:X}"
        print(f"0x{rva:08X}  {txt}{note}")

# Contexto: ~0x40 antes y ~0x30 despues del lea para ver el call que consume el string
# EN: Context: ~0x40 before and ~0x30 after the lea to see the call consuming the string (code uses 0x40/0x40)
for xr, fn in [(0x36CDC8,0x36CB80),(0x374376,0x373F00),(0x379C88,0x378A30),(0x86DD87,0x86DB80)]:
    disasm(xr-0x40, 0x80, f"XREF 0x{xr:X} en func 0x{fn:X}")
