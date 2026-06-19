# -*- coding: utf-8 -*-
# Identifica funciones clave del flujo y verifica la confusion del activeTask vtbl.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
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
    name = DATA[o:o+8].rstrip(b"\x00").decode("ascii","ignore")
    vsize = struct.unpack_from("<I", DATA, o+8)[0]
    rva = struct.unpack_from("<I", DATA, o+12)[0]
    raw_size = struct.unpack_from("<I", DATA, o+16)[0]
    raw_off = struct.unpack_from("<I", DATA, o+20)[0]
    SECTIONS.append((name, rva, vsize, raw_off, raw_size))
def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size: return raw_off + d
    return None
def which_sec(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size): return name
    return "FUERA-DE-IMAGEN"
def rd_qword(rva):
    off = rva_to_off(rva)
    return struct.unpack_from("<Q", DATA, off)[0] if off is not None else None
def rd_dword(rva):
    off = rva_to_off(rva)
    return struct.unpack_from("<I", DATA, off)[0] if off is not None else None
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

# 1) El misterio del activeTask vtbl. El valor runtime 0x7FF72A4ADD88. Si su RVA real fuera X,
#    probamos varios image bases plausibles para ver cual cae en una vtable .rdata con RTTI.
print("############ Resolver el activeTask vtbl runtime 0x7FF72A4ADD88 ############")
RUNTIME = 0x7FF72A4ADD88
# El informe asumio rva=0x6AADD88 (= runtime - 0x7FF729A40000). Probamos el offset bajo (DD88)
# contra image bases que hagan caer el RVA en .rdata (vtables). Buscamos un RVA cuyo -8 tenga COL.
# En vez de adivinar el base, buscamos en .rdata TODAS las vtables cuyo RTTI sea Task_*/Tasker/Combat
# y veremos cual termina en 0xDD88 / DC68 / E448 (coincidencia de nibbles bajos con el log).
low12_targets = {0xDD88: "activeTask(host)", 0xE448: "Task_MeleeAttack?", 0xDC68: "Tasker?"}
# escanear .rdata por punteros a COL (heuristica: buscar vtables conocidas Task)
print("  El informe restó con ImageBase ASLR equivocada. RVA estatico != runId-0x140000000.")
print("  Lo relevante: el log dice activeTaskVtbl != Task_MeleeAttack(0x16BE448) y != Tasker(0x16BDC68).")
print(f"  Task_MeleeAttack RTTI: {read_rtti_name(0x16BE448)}")
print(f"  Tasker RTTI:           {read_rtti_name(0x16BDC68)}")

# 2) Identificar funciones del flujo por RTTI/strings/prologo
print("\n############ Funciones clave del flujo melee ############")
# thunks de interes ya resueltos en flow2/3: confirmamos destinos finales
for label, t in [("attackTarget->[r11+0x68] (getCombatClass?)","vt+0x68"),
                 ("attackTarget thunk 0x204FA","0x858F40"),
                 ("attackTarget thunk 0x3E8DD (encolador)","0x6744A0"),
                 ("attackTarget thunk 0x2F220","0x436610"),
                 ("addOrder enqueue 0x791DF0","0x791DF0"),
                 ("addOrder gate 0x5D1940","0x5D1940")]:
    print(f"  {label}: {t}")

# 3) Que es 0x436610 (lo que attackTarget llama al final, modo combate)?
def disasm(start_rva, length, label):
    off = rva_to_off(start_rva)
    if off is None:
        print(f"\n=== {label}: 0x{start_rva:X} ({which_sec(start_rva)}) sin raw ==="); return
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
                tt = instr.near_branch_target - IMAGE_BASE
                rt = resolve_thunk(tt)
                mark = f"   ; CALL 0x{tt:X}" + (f" => 0x{rt:X}" if rt else "")
            except Exception: pass
        if instr.mnemonic == Mnemonic.INT3:
            print(f"0x{rva:08X}  {rawhex:<26} {fmt.format(instr)}  <-PAD"); break
        print(f"0x{rva:08X}  {rawhex:<26} {fmt.format(instr)}{mark}")

# 0x436610 = thunk 0x2F220 final de attackTarget — el "go combat" tras escribir target
disasm(0x436610, 0x40, "attackTarget tail call 0x436610")
# 0x791DF0 = encolador real de orden (addOrder)
disasm(0x791DF0, 0x80, "encolador orden 0x791DF0")
# 0x4D9C8 thunk: el que llama 0x6744A0 cuando AMBOS gates dan FALSE (el encolado real del ataque)
print("\n  thunk 0x4D9C8 =>", hex(resolve_thunk(0x4D9C8) or 0))
disasm(resolve_thunk(0x4D9C8) or 0x4D9C8, 0x80, "encolado-real-del-ataque (tras gates 0x6744A0)")
