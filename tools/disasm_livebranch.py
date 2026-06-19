# -*- coding: utf-8 -*-
# Desensamblado dirigido de la RAMA DEL CHAR VIVO (RVA 0x5CD1C0) en kenshi_x64.exe Steam 1.0.68.
# Usa iced-x86 (capstone bloqueado por WDAC). Lista CADA call (directo / vtable indirecto) y los gates jcc.
import struct, sys
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register, Instruction, FlowControl

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000

# --- Mini-parser PE para RVA<->offset ---
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

def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size:
                return raw_off + d
    return None

def off_to_rva(off):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if raw_off <= off < raw_off + raw_size:
            return srva + (off - raw_off)
    return None

def read_qword_at_rva(rva):
    o = rva_to_off(rva)
    if o is None or o+8 > len(DATA):
        return None
    return struct.unpack_from("<Q", DATA, o)[0]

# Lee el puntero (RVA destino) del slot N de una vtable cuya RVA conocemos
def vtable_slot(vtable_rva, slot_off):
    va = read_qword_at_rva(vtable_rva + slot_off)
    if va is None:
        return None
    return va - IMAGE_BASE

from iced_x86 import Register as RegEnum
_REGNAMES = {getattr(RegEnum, n): n for n in dir(RegEnum) if not n.startswith("_") and isinstance(getattr(RegEnum, n), int)}
def reg_name(r):
    return _REGNAMES.get(r, f"reg{r}")

# Resuelve thunks: si en target_rva hay un unico 'jmp rel32', devuelve el destino real.
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

START_RVA = 0x5CD1C0
MAX_LEN = 0x220  # ~544 bytes de cobertura, paramos antes si hay padding CC

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
    if instr.mnemonic == Mnemonic.INT3:
        print(f"0x{rva:08X}  {rawhex:<26} {text}   <-- PADDING CC (fin de bloque)")
        break
    instrs.append((rva, rawhex, text, instr))
    print(f"0x{rva:08X}  {rawhex:<26} {text}")

    m = instr.mnemonic
    if m == Mnemonic.CALL:
        call_sites.append((rva, instr, text))
    # jcc (saltos condicionales) — gates
    if instr.flow_control == FlowControl.CONDITIONAL_BRANCH:
        jcc_sites.append((rva, instr, text))

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
        base = instr.memory_base
        disp = instr.memory_displacement
        breg = reg_name(base)
        print(f"  0x{rva:08X}  INDIRECTO mem -> [{breg}+0x{disp:X}]   [{text}]")
    elif op0 == OpKind.REGISTER:
        print(f"  0x{rva:08X}  INDIRECTO reg -> {reg_name(instr.op0_register)}   [{text}]")
    else:
        print(f"  0x{rva:08X}  call (otro op kind {op0})   [{text}]")

print("\n" + "="*90)
print("RESUMEN DE GATES (jcc - saltos condicionales)")
print("="*90)
for rva, instr, text in jcc_sites:
    tgt = instr.near_branch_target
    trva = tgt - IMAGE_BASE
    print(f"  0x{rva:08X}  {text:<24} -> 0x{trva:X}")
