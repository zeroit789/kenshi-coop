# -*- coding: utf-8 -*-
# ES: Volcado completo de la función 0x86DB80 (candidata a asignar la facción inicial del jugador:
#     referencia "Nameless" y escribe rdi+0x790/+0x4A8) resolviendo cadenas y calls/thunks.
#     Uso: python re_func86db80.py
# EN: Full dump of function 0x86DB80 (candidate for assigning the player's initial faction: references
#     "Nameless" and writes rdi+0x790/+0x4A8) resolving strings and calls/thunks.
#     Usage: python re_func86db80.py

# Dump completo de la funcion 0x86DB80 (candidata a asignar faccion inicial del player)
# y de las 3 funciones contenedoras restantes, resolviendo strings y calls. Tambien
# clasifica 0x2134130 (contenedor del findById) y busca strings de debug en cada func.
# EN: Full dump of function 0x86DB80 (candidate to assign the player's initial faction)
#     and of the other 3 containing functions, resolving strings and calls. It also
#     classifies 0x2134130 (findById container) and looks for debug strings in each func
#     (only 0x86DB80 is dumped).
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
def read_cstr(va, maxlen=80):
    rva = va - IMAGE_BASE
    off = rva_to_off(rva)
    if off is None: return None
    end = DATA.find(b"\x00", off, off+maxlen)
    if end == -1: return None
    try:
        s = DATA[off:end].decode("ascii")
        if all(32 <= ord(c) < 127 for c in s) and len(s) >= 2: return s
    except: return None
    return None

# Resolver thunk JMP (1 instr) si target apunta a un jmp
# ES: Si en la dirección hay un JMP (thunk), devuelve su destino real (según la variante sigue uno o varios saltos).
# EN: If there is a JMP (thunk) at the address, returns its real target (one or several hops depending on the variant).
# EN: Resolve a JMP thunk (1 instr) if the target points to a jmp
def resolve_thunk(va):
    rva = va - IMAGE_BASE
    off = rva_to_off(rva)
    if off is None: return None
    dec = Decoder(64, DATA[off:off+16], ip=va)
    ins = next(iter(dec), None)
    if ins and ins.mnemonic == Mnemonic.JMP and ins.op0_kind == OpKind.NEAR_BRANCH64:
        return ins.near_branch_target
    return None

# ES: Desensambla un rango desde start_rva e imprime cada instrucción con anotaciones (destinos, cadenas o marcas según el script).
# EN: Disassembles a range from start_rva and prints each instruction with annotations (targets, strings or marks depending on the script).
def disasm(start_rva, end_rva, label, max_instr=400):
    off = rva_to_off(start_rva)
    length = end_rva - start_rva
    code = DATA[off:off+length]
    dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n========== {label} (0x{start_rva:X}..0x{end_rva:X}) ==========")
    cnt = 0
    for instr in dec:
        cnt += 1
        if cnt > max_instr: print("  ...(truncado)"); break
        rva = instr.ip - IMAGE_BASE
        txt = fmt.format(instr)
        note = ""
        if instr.memory_base == Register.RIP:
            s = read_cstr(instr.memory_displacement)
            if s: note = f'   ; "{s}"'
            else: note = f"   ; [abs 0x{instr.memory_displacement-IMAGE_BASE:X}]"
        if instr.flow_control == FlowControl.CALL and instr.op0_kind == OpKind.NEAR_BRANCH64:
            t = instr.near_branch_target
            th = resolve_thunk(t)
            if th: note = f"   ; -> thunk->0x{th-IMAGE_BASE:X}"
            else:  note = f"   ; -> sub_0x{t-IMAGE_BASE:X}"
        print(f"0x{rva:08X}  {txt}{note}")

# Dump completo de la funcion clave 0x86DB80
# EN: Full dump of the key function 0x86DB80
disasm(0x86DB80, 0x86DFCD, "FUNC 0x86DB80 (xref + 'Nameless'; escribe rdi+0x790/+0x4A8)")
