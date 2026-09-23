# -*- coding: utf-8 -*-
# ES: Desensamblador de una función con anotaciones (solo lectura): resuelve destinos de call (y
#     thunks JMP), destinos de saltos, marca accesos a la instancia GameWorld (RVA 0x2134110 en .data)
#     y a .data, y pone el nombre RTTI cuando un LEA carga una vtable. Parsea las secciones PE a mano.
#     Uso: python re_actionlist.py <rva_hex> [longitud_hex=0x200] [etiqueta]
# EN: Annotated single-function disassembler (read-only): resolves call targets (and JMP thunks),
#     branch targets, marks accesses to the GameWorld instance (RVA 0x2134110 in .data) and to .data,
#     and adds the RTTI name when a LEA loads a vtable. Parses the PE sections by hand.
#     Usage: python re_actionlist.py <rva_hex> [length_hex=0x200] [label]

# READ-ONLY RE Kenshi Steam 1.0.68 — angulo: lista/registro secundario de acciones + gates de estado
# Desensambla con resolucion de thunks JMP, RTTI en LEA rip, y marca refs a GameWorld 0x2134110.
# EN: READ-ONLY RE Kenshi Steam 1.0.68 - angle: secondary action list/registry + state gates
#     Disassembles resolving JMP thunks, RTTI on rip LEA, and marks refs to GameWorld 0x2134110.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
GW = 0x2134110  # instancia GameWorld embebida en .data
with open(EXE, "rb") as f: DATA = f.read()
# ES: Tabla de secciones leída directamente de las cabeceras PE (sin pefile).
# EN: Section table read directly from the PE headers (without pefile).
e_lfanew = struct.unpack_from("<I", DATA, 0x3C)[0]; coff = e_lfanew + 4
num_sec = struct.unpack_from("<H", DATA, coff + 2)[0]
opt_size = struct.unpack_from("<H", DATA, coff + 16)[0]
sec_off = coff + 20 + opt_size
SECTIONS = []
for i in range(num_sec):
    o = sec_off + i*40
    name = DATA[o:o+8].rstrip(b"\x00").decode("ascii","ignore")
    vsize = struct.unpack_from("<I", DATA, o+8)[0]; rva = struct.unpack_from("<I", DATA, o+12)[0]
    raw_size = struct.unpack_from("<I", DATA, o+16)[0]; raw_off = struct.unpack_from("<I", DATA, o+20)[0]
    SECTIONS.append((name, rva, vsize, raw_off, raw_size))
# ES: RVA -> offset de fichero (None si no tiene datos en disco).
# EN: RVA -> file offset (None if it has no on-disk data).
def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size: return raw_off + d
    return None
# ES: Nombre de la sección que contiene el RVA.
# EN: Name of the section containing the RVA.
def which_sec(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size): return name
    return "FUERA"
# ES: Lectura de qword/dword por RVA.
# EN: qword/dword read by RVA.
def rd_qword(rva):
    off = rva_to_off(rva); return struct.unpack_from("<Q", DATA, off)[0] if off is not None else None
def rd_dword(rva):
    off = rva_to_off(rva); return struct.unpack_from("<I", DATA, off)[0] if off is not None else None
# ES: Si en rva hay un JMP incondicional (thunk), devuelve su destino.
# EN: If there is an unconditional JMP (thunk) at rva, returns its target.
def resolve_thunk(rva):
    off = rva_to_off(rva)
    if off is None: return None
    dec = Decoder(64, DATA[off:off+16], ip=IMAGE_BASE+rva)
    try: instr = next(iter(dec))
    except Exception: return None
    if instr.mnemonic == Mnemonic.JMP and instr.flow_control == FlowControl.UNCONDITIONAL_BRANCH:
        try: return instr.near_branch_target - IMAGE_BASE
        except Exception: return None
    return None
# ES: Nombre RTTI de la vtable (COL en vt-8, TypeDescriptor en COL+0xC, nombre en TD+0x10).
# EN: RTTI name of the vtable (COL at vt-8, TypeDescriptor at COL+0xC, name at TD+0x10).
def read_rtti_name(vtbl_rva):
    col_ptr = rd_qword(vtbl_rva - 8)
    if not col_ptr: return None
    col_rva = col_ptr - IMAGE_BASE
    td_rva = rd_dword(col_rva + 0xC)
    if not td_rva: return None
    name_off = rva_to_off(td_rva + 0x10)
    if name_off is None: return None
    try: end = DATA.index(b"\x00", name_off)
    except ValueError: return None
    return DATA[name_off:end].decode("ascii","ignore")

# ES: Desensambla length bytes desde start_rva con anotaciones; se para en el primer int3.
# EN: Disassembles length bytes from start_rva with annotations; stops at the first int3.
def disasm(start_rva, length, label, follow=True):
    off = rva_to_off(start_rva)
    if off is None: print(f"\n=== {label} 0x{start_rva:X} sin raw ==="); return
    code = DATA[off:off+length]; dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} 0x{start_rva:X} ({which_sec(start_rva)}) ===")
    for instr in dec:
        rva = instr.ip - IMAGE_BASE
        mark = ""
        if instr.flow_control == FlowControl.CALL:
            try:
                tt = instr.near_branch_target - IMAGE_BASE; rt = resolve_thunk(tt)
                mark = f"   ; CALL 0x{tt:X}" + (f" =>0x{rt:X}" if rt else "")
            except Exception: pass
        elif instr.flow_control in (FlowControl.CONDITIONAL_BRANCH, FlowControl.UNCONDITIONAL_BRANCH):
            try: mark = f"   ; -> 0x{instr.near_branch_target - IMAGE_BASE:X}"
            except Exception: pass
        # refs a memoria absoluta: marcar GameWorld y offsets
        # EN: absolute memory refs: mark GameWorld and offsets
        try:
            if instr.memory_base == 0 and instr.memory_index == 0:  # rip o abs
                t = instr.memory_displacement
                tr = t - IMAGE_BASE
                if t == GW + IMAGE_BASE or (GW <= tr <= GW+0x1000):
                    mark += f"   ; GW+0x{tr-GW:X}"
                elif which_sec(tr) == ".data":
                    mark += f"   ; .data 0x{tr:X}"
        except Exception: pass
        if instr.mnemonic == Mnemonic.LEA:
            try:
                t = instr.memory_displacement - IMAGE_BASE
                nm = read_rtti_name(t)
                if nm: mark += f"   ; VTBL {nm}"
                if t == GW: mark += "   ; &GameWorld"
            except Exception: pass
        if instr.mnemonic == Mnemonic.INT3: break
        print(f"0x{rva:08X}  {fmt.format(instr)}{mark}")

# ES: Punto de entrada por línea de comandos.
# EN: Command-line entry point.
if __name__ == "__main__":
    import sys
    rva = int(sys.argv[1],16); length = int(sys.argv[2],16) if len(sys.argv)>2 else 0x200
    label = sys.argv[3] if len(sys.argv)>3 else "func"
    disasm(rva, length, label)
