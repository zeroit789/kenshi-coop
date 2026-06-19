# -*- coding: utf-8 -*-
# Desensambla contexto alrededor de cada xref del literal "204-gamedata.base"
# y resuelve targets de call/lea a strings cercanos para identificar la funcion.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register, FlowControl

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
        if instr.memory_base == Register.RIP:
            s = read_cstr(instr.memory_displacement)
            if s: note = f'   ; "{s}"'
        # resolver call directo
        if instr.flow_control == FlowControl.CALL and instr.op0_kind == OpKind.NEAR_BRANCH64:
            t = instr.near_branch_target - IMAGE_BASE
            note = f"   ; -> sub_0x{t:X}"
        print(f"0x{rva:08X}  {txt}{note}")

# Contexto: ~0x40 antes y ~0x30 despues del lea para ver el call que consume el string
for xr, fn in [(0x36CDC8,0x36CB80),(0x374376,0x373F00),(0x379C88,0x378A30),(0x86DD87,0x86DB80)]:
    disasm(xr-0x40, 0x80, f"XREF 0x{xr:X} en func 0x{fn:X}")
