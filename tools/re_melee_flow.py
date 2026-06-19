# -*- coding: utf-8 -*-
# RE del flujo de attackTarget -> encolador para entender por que el melee no se encola.
# Desensambla: attackTarget 0x5CB0A0, encolador 0x6744A0 / 0x674300, addOrder 0x5D20D0,
# addOrderSelectedCharacters 0x7F8BC0. Resuelve la vtbl activeTask 0x6AADD88.
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
            if d < raw_size:
                return raw_off + d
    return None

def rd_qword(rva):
    off = rva_to_off(rva)
    if off is None: return None
    return struct.unpack_from("<Q", DATA, off)[0]

def disasm(start_rva, length, label, stop_on_ret=False, calls_out=None):
    off = rva_to_off(start_rva)
    if off is None:
        print(f"\n=== {label}: RVA 0x{start_rva:X} sin mapear ===")
        return
    code = DATA[off:off+length]
    dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} desde RVA 0x{start_rva:X} ===")
    for instr in dec:
        rva = instr.ip - IMAGE_BASE
        raw = code[instr.ip-(IMAGE_BASE+start_rva):instr.ip-(IMAGE_BASE+start_rva)+instr.len]
        rawhex = " ".join(f"{b:02x}" for b in raw)
        mark = ""
        if instr.flow_control in (FlowControl.CONDITIONAL_BRANCH, FlowControl.UNCONDITIONAL_BRANCH):
            t = instr.near_branch_target - IMAGE_BASE
            mark = f"   ; -> 0x{t:X}"
        if instr.flow_control == FlowControl.CALL:
            try:
                t = instr.near_branch_target - IMAGE_BASE
                mark = f"   ; CALL 0x{t:X}"
                if calls_out is not None: calls_out.append((rva, t))
            except Exception:
                pass
        if instr.mnemonic == Mnemonic.INT3:
            print(f"0x{rva:08X}  {rawhex:<28} {fmt.format(instr)}  <-- PADDING")
            if stop_on_ret: break
        else:
            print(f"0x{rva:08X}  {rawhex:<28} {fmt.format(instr)}{mark}")
        if stop_on_ret and instr.mnemonic == Mnemonic.RET:
            break

# ── Resolver thunk JMP (slot de vtable apunta a `jmp <real>`) ──
def resolve_thunk(rva):
    off = rva_to_off(rva)
    if off is None: return None
    b = DATA[off:off+16]
    dec = Decoder(64, b, ip=IMAGE_BASE+rva)
    instr = next(iter(dec))
    if instr.mnemonic == Mnemonic.JMP and instr.flow_control == FlowControl.UNCONDITIONAL_BRANCH:
        try:
            return instr.near_branch_target - IMAGE_BASE
        except Exception:
            return None
    return None

# ── 1) Identificar la vtbl del activeTask 0x6AADD88 ──
print("############ IDENT vtbl activeTask 0x6AADD88 ############")
for vt in [0x6AADD88, 0x6AAE448, 0x6AADC68]:
    print(f"\n--- vtbl RVA 0x{vt:X} (slots 0..6) ---")
    for s in range(0, 7):
        slot_rva = vt + s*8
        q = rd_qword(slot_rva)
        if q is None:
            print(f"  slot+0x{s*8:02X}: <sin mapear>"); continue
        fn = q - IMAGE_BASE
        real = resolve_thunk(fn)
        extra = f" -> thunk a 0x{real:X}" if real else ""
        print(f"  slot+0x{s*8:02X}: 0x{q:X} (RVA 0x{fn:X}){extra}")
# Tambien leer el COL (vt-8) que apunta a RTTI CompleteObjectLocator -> TypeDescriptor
for vt in [0x6AADD88, 0x6AAE448, 0x6AADC68]:
    col = rd_qword(vt - 8)
    print(f"\n  vtbl 0x{vt:X}: COL(vt-8) = 0x{col:X}" if col else f"\n  vtbl 0x{vt:X}: COL n/d")

# ── 2) attackTarget 0x5CB0A0 (flujo completo hasta ret) ──
calls = []
disasm(0x5CB0A0, 0x200, "attackTarget Character::attackTarget", stop_on_ret=False, calls_out=calls)
print("\n  -- CALLs de attackTarget --")
for r, t in calls:
    print(f"     0x{r:X} -> 0x{t:X}")

# ── 3) encolador 0x6744A0 (los 2 gates isAlly) y 0x674300 ──
calls2 = []
disasm(0x6744A0, 0x140, "encolador 0x6744A0 (modo 4, 2 gates isAlly)", calls_out=calls2)
print("\n  -- CALLs de 0x6744A0 --")
for r, t in calls2:
    print(f"     0x{r:X} -> 0x{t:X}")

calls3 = []
disasm(0x674300, 0x1B0, "encolador-padre 0x674300", calls_out=calls3)
print("\n  -- CALLs de 0x674300 --")
for r, t in calls3:
    print(f"     0x{r:X} -> 0x{t:X}")

# ── 4) addOrder 0x5D20D0 y addOrderSelectedCharacters 0x7F8BC0 ──
calls4 = []
disasm(0x5D20D0, 0x150, "addOrder Character::addOrder", calls_out=calls4)
print("\n  -- CALLs de 0x5D20D0 --")
for r, t in calls4:
    print(f"     0x{r:X} -> 0x{t:X}")

calls5 = []
disasm(0x7F8BC0, 0x180, "addOrderSelectedCharacters 0x7F8BC0", calls_out=calls5)
print("\n  -- CALLs de 0x7F8BC0 --")
for r, t in calls5:
    print(f"     0x{r:X} -> 0x{t:X}")
