# -*- coding: utf-8 -*-
# ES: Comprueba qué hay realmente en 0x6AADD88 (el "vtbl" de activeTask reportado; su sección) y
#     desensambla el resto de attackTarget (0x5CB29E), addOrder (0x5D20D0), addOrderSelectedCharacters
#     (0x7F8BC0) y el encolador padre 0x674300. Uso: python re_melee_flow3.py
# EN: Checks what really is at 0x6AADD88 (the reported activeTask "vtbl"; its section) and disassembles the
#     rest of attackTarget (0x5CB29E), addOrder (0x5D20D0), addOrderSelectedCharacters (0x7F8BC0) and the
#     parent enqueuer 0x674300. Usage: python re_melee_flow3.py

import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl

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
# ES: Nombre de la sección que contiene el RVA.
# EN: Name of the section containing the RVA.
def which_sec(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size): return name
    return "?"
# ES: Lee un qword (8 bytes) en un RVA.
# EN: Reads a qword (8 bytes) at an RVA.
def rd_qword(rva):
    off = rva_to_off(rva)
    return struct.unpack_from("<Q", DATA, off)[0] if off is not None else None
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

# ES: Desensambla un rango desde start_rva e imprime cada instrucción con anotaciones (destinos, cadenas o marcas según el script).
# EN: Disassembles a range from start_rva and prints each instruction with annotations (targets, strings or marks depending on the script).
def disasm(start_rva, length, label):
    off = rva_to_off(start_rva)
    if off is None:
        print(f"\n=== {label}: 0x{start_rva:X} sin mapear ==="); return
    code = DATA[off:off+length]
    dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} 0x{start_rva:X} ({which_sec(start_rva)}) ===")
    for instr in dec:
        rva = instr.ip - IMAGE_BASE
        raw = code[instr.ip-(IMAGE_BASE+start_rva):instr.ip-(IMAGE_BASE+start_rva)+instr.len]
        rawhex = " ".join(f"{b:02x}" for b in raw)
        mark = ""
        if instr.flow_control in (FlowControl.CONDITIONAL_BRANCH, FlowControl.UNCONDITIONAL_BRANCH):
            try: mark = f"   ; -> 0x{instr.near_branch_target - IMAGE_BASE:X}"
            except Exception: pass
        if instr.flow_control == FlowControl.CALL:
            try:
                t = instr.near_branch_target - IMAGE_BASE
                rt = resolve_thunk(t)
                mark = f"   ; CALL 0x{t:X}" + (f" => 0x{rt:X}" if rt else "")
            except Exception: pass
        if instr.mnemonic == Mnemonic.INT3:
            print(f"0x{rva:08X}  {rawhex:<26} {fmt.format(instr)}  <-PAD"); break
        print(f"0x{rva:08X}  {rawhex:<26} {fmt.format(instr)}{mark}")

# Que hay EXACTAMENTE en 0x6AADD88 (el "vtbl" del activeTask del host)
# EN: What EXACTLY is at 0x6AADD88 (the host's activeTask "vtbl")
print("############ Que es 0x6AADD88 (activeTask vtbl reportado) ############")
print(f"  seccion de 0x6AADD88 = {which_sec(0x6AADD88)}")
disasm(0x6AADD88, 0x40, "contenido en RVA 0x6AADD88")
# Y leerlo como puntero por si fuera dato
# EN: And read it as a pointer in case it is data
q = rd_qword(0x6AADD88)
print(f"\n  qword@0x6AADD88 = {'0x%X'%q if q else 'n/d'}")

# resto de attackTarget (0x5CB29E..)
# EN: rest of attackTarget (0x5CB29E..)
disasm(0x5CB29E, 0x70, "attackTarget resto")

# addOrder 0x5D20D0 (flujo, ver si llama a 0x6744A0 con que modo)
# EN: addOrder 0x5D20D0 (flow; see whether it calls 0x6744A0 and with which mode)
disasm(0x5D20D0, 0x160, "addOrder Character::addOrder")

# addOrderSelectedCharacters 0x7F8BC0
# EN: addOrderSelectedCharacters 0x7F8BC0
disasm(0x7F8BC0, 0x180, "addOrderSelectedCharacters")

# El encolador-padre 0x674300 (lo que llama a 0x6744A0)
# EN: The parent enqueuer 0x674300 (what calls 0x6744A0)
disasm(0x674300, 0x1A0, "0x674300 (padre del encolador)")
