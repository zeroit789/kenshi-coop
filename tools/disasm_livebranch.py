# -*- coding: utf-8 -*-
# ES: Desensamblado dirigido de la rama del personaje vivo (RVA 0x5CD1C0) de kenshi_x64.exe Steam 1.0.68:
#     imprime hasta 0x220 bytes (para en el relleno int3) y después resume cada call (directo con thunk
#     resuelto, indirecto por memoria/vtable o por registro) y cada salto condicional ("gates").
#     Uso: python disasm_livebranch.py
# EN: Targeted disassembly of the live-character branch (RVA 0x5CD1C0) of kenshi_x64.exe Steam 1.0.68:
#     prints up to 0x220 bytes (stops at the int3 padding) and then summarizes every call (direct with
#     resolved thunk, indirect through memory/vtable or through a register) and every conditional jump
#     ("gates"). Usage: python disasm_livebranch.py

# Desensamblado dirigido de la RAMA DEL CHAR VIVO (RVA 0x5CD1C0) en kenshi_x64.exe Steam 1.0.68.
# Usa iced-x86 (capstone bloqueado por WDAC). Lista CADA call (directo / vtable indirecto) y los gates jcc.
# EN: Targeted disassembly of the LIVE CHAR BRANCH (RVA 0x5CD1C0) in kenshi_x64.exe Steam 1.0.68.
#     Uses iced-x86 (capstone blocked by WDAC). Lists EVERY call (direct / indirect vtable) and the jcc gates.
import struct, sys
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register, Instruction, FlowControl

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000

# --- Mini-parser PE para RVA<->offset ---
# EN: --- Mini PE parser for RVA<->offset ---
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
    name = DATA[o:o+8].rstrip(b"\x00").decode("ascii", "ignore")
    vsize = struct.unpack_from("<I", DATA, o+8)[0]
    rva = struct.unpack_from("<I", DATA, o+12)[0]
    raw_size = struct.unpack_from("<I", DATA, o+16)[0]
    raw_off = struct.unpack_from("<I", DATA, o+20)[0]
    SECTIONS.append((name, rva, vsize, raw_off, raw_size))

# ES: RVA -> offset en el fichero.
# EN: RVA -> file offset.
def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size:
                return raw_off + d
    return None

# ES: Offset de fichero -> RVA.
# EN: File offset -> RVA.
def off_to_rva(off):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if raw_off <= off < raw_off + raw_size:
            return srva + (off - raw_off)
    return None

# ES: Lee un qword en un RVA (None si queda fuera).
# EN: Reads a qword at an RVA (None if out of range).
def read_qword_at_rva(rva):
    o = rva_to_off(rva)
    if o is None or o+8 > len(DATA):
        return None
    return struct.unpack_from("<Q", DATA, o)[0]

# Lee el puntero (RVA destino) del slot N de una vtable cuya RVA conocemos
# EN: Reads the pointer (target RVA) of slot N of a vtable whose RVA we know
def vtable_slot(vtable_rva, slot_off):
    va = read_qword_at_rva(vtable_rva + slot_off)
    if va is None:
        return None
    return va - IMAGE_BASE

# ES: Mapa valor -> nombre de registro de iced_x86.
# EN: iced_x86 register value -> name map.
from iced_x86 import Register as RegEnum
_REGNAMES = {getattr(RegEnum, n): n for n in dir(RegEnum) if not n.startswith("_") and isinstance(getattr(RegEnum, n), int)}
def reg_name(r):
    return _REGNAMES.get(r, f"reg{r}")

# Resuelve thunks: si en target_rva hay un unico 'jmp rel32', devuelve el destino real.
# EN: Resolves thunks: if target_rva holds a single 'jmp rel32', returns the real target.
def resolve_thunk(target_rva, depth=0):
    if depth > 4:
        return target_rva, ""
    o = rva_to_off(target_rva)
    if o is None:
        return target_rva, ""
    dec = Decoder(64, DATA[o:o+16], ip=IMAGE_BASE+target_rva)
    try:
        ins = next(iter(dec))
    except StopIteration:
        return target_rva, ""
    if ins.mnemonic == Mnemonic.JMP and ins.op0_kind in (OpKind.NEAR_BRANCH64, OpKind.NEAR_BRANCH32):
        real = ins.near_branch_target - IMAGE_BASE
        r2, _ = resolve_thunk(real, depth+1)
        return r2, f" (thunk -> 0x{r2:X})"
    return target_rva, ""

# ES: Parámetros: inicio de la rama y bytes máximos a cubrir.
# EN: Parameters: branch start and maximum bytes to cover.
START_RVA = 0x5CD1C0
MAX_LEN = 0x220  # ~544 bytes de cobertura, paramos antes si hay padding CC
# EN: ~544 bytes of coverage, we stop earlier if there is CC padding

# ES: Preparar decodificador y formateador Intel.
# EN: Set up the decoder and the Intel formatter.
start_off = rva_to_off(START_RVA)
code = DATA[start_off:start_off+MAX_LEN]
decoder = Decoder(64, code, ip=IMAGE_BASE + START_RVA)
fmt = Formatter(FormatterSyntax.INTEL)
fmt.hex_prefix = "0x"
fmt.hex_suffix = ""
fmt.uppercase_hex = False
fmt.space_after_operand_separator = True

print(f"=== Desensamblado RAMA CHAR VIVO desde RVA 0x{START_RVA:X} (Steam 1.0.68) ===")
print(f"{'RVA':>10}  {'bytes':<26} instr")
print("-"*90)

call_sites = []
jcc_sites = []
instrs = []

# ES: Pasada principal: imprime cada instrucción y guarda calls y saltos condicionales.
# EN: Main pass: prints every instruction and stores calls and conditional jumps.
stop = False
for instr in decoder:
    if stop:
        break
    ip = instr.ip
    rva = ip - IMAGE_BASE
    raw = code[ip - (IMAGE_BASE+START_RVA): ip - (IMAGE_BASE+START_RVA) + instr.len]
    rawhex = " ".join(f"{b:02x}" for b in raw)
    text = fmt.format(instr)
    # Parar si llegamos a padding int3 (alineamiento entre funciones)
    # EN: Stop when we reach int3 padding (alignment between functions)
    if instr.mnemonic == Mnemonic.INT3:
        print(f"0x{rva:08X}  {rawhex:<26} {text}   <-- PADDING CC (fin de bloque)")
        break
    instrs.append((rva, rawhex, text, instr))
    print(f"0x{rva:08X}  {rawhex:<26} {text}")

    m = instr.mnemonic
    if m == Mnemonic.CALL:
        call_sites.append((rva, instr, text))
    # jcc (saltos condicionales) — gates
    # EN: jcc (conditional jumps) - gates
    if instr.flow_control == FlowControl.CONDITIONAL_BRANCH:
        jcc_sites.append((rva, instr, text))

# ES: Resumen de llamadas: directas (thunk resuelto), indirectas por memoria (posible vtable) o por registro.
# EN: Call summary: direct (thunk resolved), indirect through memory (possible vtable) or through a register.
print("\n" + "="*90)
print("RESUMEN DE LLAMADAS (call)")
print("="*90)
for rva, instr, text in call_sites:
    op0 = instr.op0_kind
    if op0 in (OpKind.NEAR_BRANCH64, OpKind.NEAR_BRANCH32, OpKind.NEAR_BRANCH16):
        tgt = instr.near_branch_target
        trva = tgt - IMAGE_BASE
        real, note = resolve_thunk(trva)
        print(f"  0x{rva:08X}  DIRECTO -> RVA 0x{trva:X}{note}   [{text}]")
    elif op0 == OpKind.MEMORY:
        # call [base + disp]  -> indirecto (posible vtable)
        # EN: call [base + disp] -> indirect (possible vtable)
        base = instr.memory_base
        disp = instr.memory_displacement
        breg = reg_name(base)
        print(f"  0x{rva:08X}  INDIRECTO mem -> [{breg}+0x{disp:X}]   [{text}]")
    elif op0 == OpKind.REGISTER:
        print(f"  0x{rva:08X}  INDIRECTO reg -> {reg_name(instr.op0_register)}   [{text}]")
    else:
        print(f"  0x{rva:08X}  call (otro op kind {op0})   [{text}]")

# ES: Resumen de saltos condicionales ("gates") y su destino.
# EN: Summary of conditional jumps ("gates") and their target.
print("\n" + "="*90)
print("RESUMEN DE GATES (jcc - saltos condicionales)")
print("="*90)
for rva, instr, text in jcc_sites:
    tgt = instr.near_branch_target
    trva = tgt - IMAGE_BASE
    print(f"  0x{rva:08X}  {text:<24} -> 0x{trva:X}")
