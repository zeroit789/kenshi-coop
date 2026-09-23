# -*- coding: utf-8 -*-
# ES: Desensambla el tramo 0x5CCDF0-0x5CCE50 del tick de IA (0x5CCD90) alrededor del "gate" cmp +0x5BC /
#     je para confirmar que el salto va a la rama del personaje vivo 0x5CD1C0. Lee kenshi_x64.exe y
#     parsea las secciones PE a mano. Uso: python disasm_context.py
# EN: Disassembles the 0x5CCDF0-0x5CCE50 stretch of the AI tick (0x5CCD90) around the cmp +0x5BC / je
#     "gate" to confirm the jump goes to the live-character branch 0x5CD1C0. Reads kenshi_x64.exe and
#     parses the PE sections by hand. Usage: python disasm_context.py

# Desensambla el AI tick 0x5CCD90 alrededor del je hacia 0x5CD1C0 para confirmar el destino exacto.
# EN: Disassembles the AI tick 0x5CCD90 around the je to 0x5CD1C0 to confirm the exact target.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register, FlowControl

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
with open(EXE, "rb") as f:
    DATA = f.read()
# ES: Tabla de secciones leída de las cabeceras PE (e_lfanew -> COFF -> cabeceras de 40 bytes).
# EN: Section table read from the PE headers (e_lfanew -> COFF -> 40-byte headers).
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
# ES: RVA -> offset en el fichero (None si no tiene datos en disco).
# EN: RVA -> file offset (None if it has no on-disk data).
def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size:
                return raw_off + d
    return None

# ES: Desensambla length bytes desde start_rva marcando los saltos condicionales (y el que va a la rama viva); para en int3.
# EN: Disassembles length bytes from start_rva flagging conditional jumps (and the one to the live branch); stops at int3.
def disasm(start_rva, length, label):
    off = rva_to_off(start_rva)
    code = DATA[off:off+length]
    dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} desde RVA 0x{start_rva:X} ===")
    for instr in dec:
        rva = instr.ip - IMAGE_BASE
        raw = code[instr.ip-(IMAGE_BASE+start_rva):instr.ip-(IMAGE_BASE+start_rva)+instr.len]
        rawhex = " ".join(f"{b:02x}" for b in raw)
        mark = ""
        if instr.flow_control == FlowControl.CONDITIONAL_BRANCH:
            t = instr.near_branch_target - IMAGE_BASE
            mark = f"   ; -> 0x{t:X}" + ("  <== SALTO A RAMA VIVA" if t==0x5CD1C0 else "")
        if instr.mnemonic == Mnemonic.INT3:
            print(f"0x{rva:08X}  {rawhex:<24} {fmt.format(instr)}  <-- PADDING"); break
        print(f"0x{rva:08X}  {rawhex:<24} {fmt.format(instr)}{mark}")

# Tramo del AI tick alrededor del gate (0x5CCE00..0x5CCE40 contiene el cmp +0x5BC / je)
# EN: AI tick stretch around the gate (0x5CCE00..0x5CCE40 contains the cmp +0x5BC / je)
disasm(0x5CCDF0, 0x60, "AI tick 0x5CCD90 - zona del gate +0x5BC")
