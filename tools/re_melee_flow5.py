# -*- coding: utf-8 -*-
# ES: RE detallado de cuatro preguntas del flujo melee: (1) 0x674300 completo, la factory 0x5E8410 y
#     0x66CC10; (2) setAttackTarget 0x665580 (escribe CombatClass+0x290); (3) la rama 0x67451E de
#     0x6744A0 (salida sin encolar); (4) el bloque 0x5CB150 y el prólogo de attackTarget.
#     Uso: python re_melee_flow5.py
# EN: Detailed RE of four melee-flow questions: (1) the whole 0x674300, the 0x5E8410 factory and 0x66CC10;
#     (2) setAttackTarget 0x665580 (writes CombatClass+0x290); (3) branch 0x67451E of 0x6744A0 (exit
#     without enqueuing); (4) block 0x5CB150 and the attackTarget prologue. Usage: python re_melee_flow5.py

# RE detallado para responder las 4 preguntas de Zero:
#  1) 0x674300 completo + identificar 0x5E8410 (factory/allocator) y el objeto orden, + 0x66CC10
#  2) setAttackTarget 0x665580 (escribe CombatClass+0x290) y si attackTarget lo llama
#  3) rama 0x67451E (salida sin encolar de 0x6744A0)
#  4) segundo bloque 0x5CB150 (push rbx/rbp/rsi/rdi/r12/r13) iterando arrays
# EN: Detailed RE answering the 4 questions:
#      1) full 0x674300 + identify 0x5E8410 (factory/allocator) and the order object, + 0x66CC10
#      2) setAttackTarget 0x665580 (writes CombatClass+0x290) and whether attackTarget calls it
#      3) branch 0x67451E (exit without enqueuing from 0x6744A0)
#      4) second block 0x5CB150 (push rbx/rbp/rsi/rdi/r12/r13) iterating arrays
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl, OpKind, Register

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

# ES: Nombre de la sección que contiene el RVA.
# EN: Name of the section containing the RVA.
def which_sec(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            return name
    return "FUERA"

# ES: Lee un qword (8 bytes) en un RVA.
# EN: Reads a qword (8 bytes) at an RVA.
def rd_qword(rva):
    off = rva_to_off(rva)
    return struct.unpack_from("<Q", DATA, off)[0] if off is not None else None

# ES: Lee un dword (4 bytes) en un RVA.
# EN: Reads a dword (4 bytes) at an RVA.
def rd_dword(rva):
    off = rva_to_off(rva)
    return struct.unpack_from("<I", DATA, off)[0] if off is not None else None

# ES: Si en la dirección hay un JMP (thunk), devuelve su destino real (según la variante sigue uno o varios saltos).
# EN: If there is a JMP (thunk) at the address, returns its real target (one or several hops depending on the variant).
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

# ES: Nombre RTTI de la vtable (COL en vt-8 -> TD -> nombre), o None.
# EN: RTTI name of the vtable (COL at vt-8 -> TD -> name), or None.
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

# ES: Desensambla un rango desde start_rva e imprime cada instrucción con anotaciones (destinos, cadenas o marcas según el script).
# EN: Disassembles a range from start_rva and prints each instruction with annotations (targets, strings or marks depending on the script).
def disasm(start_rva, length, label, stop_on_ret=False, max_instr=9999):
    off = rva_to_off(start_rva)
    if off is None:
        print(f"\n=== {label}: 0x{start_rva:X} ({which_sec(start_rva)}) sin raw ==="); return
    code = DATA[off:off+length]
    dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} 0x{start_rva:X} ({which_sec(start_rva)}) ===")
    n = 0
    for instr in dec:
        n += 1
        if n > max_instr: break
        rva = instr.ip - IMAGE_BASE
        base = instr.ip-(IMAGE_BASE+start_rva)
        raw = code[base:base+instr.len]
        rawhex = " ".join(f"{b:02x}" for b in raw)
        mark = ""
        if instr.flow_control in (FlowControl.CONDITIONAL_BRANCH, FlowControl.UNCONDITIONAL_BRANCH):
            try: mark = f"   ; -> 0x{instr.near_branch_target - IMAGE_BASE:X}"
            except Exception: pass
        if instr.flow_control == FlowControl.CALL:
            try:
                tt = instr.near_branch_target - IMAGE_BASE
                rt = resolve_thunk(tt)
                mark = f"   ; CALL 0x{tt:X}" + (f" => 0x{rt:X}" if rt else "")
            except Exception: pass
        if instr.mnemonic == Mnemonic.INT3:
            print(f"0x{rva:08X}  {rawhex:<28} {fmt.format(instr)}  <-PAD")
            if stop_on_ret: break
        else:
            print(f"0x{rva:08X}  {rawhex:<28} {fmt.format(instr)}{mark}")
        if stop_on_ret and instr.mnemonic == Mnemonic.RET:
            break

# ===== PREGUNTA 1: 0x674300 completo =====
# EN: ===== QUESTION 1: full 0x674300 =====
disasm(0x674300, 0x1A0, "P1: encolado real 0x674300", stop_on_ret=False)

# 0x5E8410: que construye? Mirar su cuerpo, buscar el call al allocator (operator new) y la
# escritura de vtable [rax]=lea vtbl. Detectar mov [rax], rcx con vtable rdata.
# EN: 0x5E8410: what does it build? Look at its body, find the call to the allocator (operator new) and the
#     vtable write [rax]=lea vtbl. Detect mov [rax], rcx with an .rdata vtable.
disasm(0x5E8410, 0x120, "P1: factory/allocator 0x5E8410")

# 0x66CC10: push a la cola?
# EN: 0x66CC10: push to the queue?
disasm(0x66CC10, 0xC0, "P1: 0x66CC10 (push a la cola del char?)")

# vtables candidatas para el objeto orden
# EN: candidate vtables for the order object
print("\n## RTTI vtables candidatas ##")
for vt in [0x16BE448, 0x16BDC68]:
    print(f"  vtbl 0x{vt:X}: {read_rtti_name(vt)}")

# ===== PREGUNTA 2: setAttackTarget 0x665580 =====
# EN: ===== QUESTION 2: setAttackTarget 0x665580 =====
disasm(0x665580, 0x120, "P2: setAttackTarget 0x665580 (escribe CombatClass+0x290)")

# ===== PREGUNTA 3: rama 0x67451E (dentro de 0x6744A0) =====
# EN: ===== QUESTION 3: branch 0x67451E (inside 0x6744A0) =====
disasm(0x6744A0, 0x180, "P3: encolador 0x6744A0 completo (ver rama 0x67451E)")

# ===== PREGUNTA 4: 0x5CB150 segundo bloque =====
# EN: ===== QUESTION 4: second block 0x5CB150 =====
disasm(0x5CB150, 0x180, "P4: bloque 0x5CB150 (itera arrays CharBody+0x648, AI+0x650)")
# Tambien ver attackTarget 0x5CB0A0 para ver si cae/llama a 0x5CB150
# EN: Also look at attackTarget 0x5CB0A0 to see whether it falls into/calls 0x5CB150
disasm(0x5CB0A0, 0xB0, "P4b: attackTarget 0x5CB0A0 prologo (ver si fluye a 0x5CB150)")
