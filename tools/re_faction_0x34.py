# -*- coding: utf-8 -*-
# Verificacion del campo Faction+0x34 (fundamentalNPCType / CharacterTypeEnum).
# Desensambla: getter getFundamentalNPCType (0x289F20), ctor Faction (0x800E00),
# setup (0x7FD010). Busca lecturas/escrituras de [reg+0x34].
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

def disasm(start_rva, length, label, stop_at_ret=False):
    off = rva_to_off(start_rva)
    code = DATA[off:off+length]
    dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} desde RVA 0x{start_rva:X} ===")
    base = IMAGE_BASE+start_rva
    for instr in dec:
        rva = instr.ip - IMAGE_BASE
        raw = code[instr.ip-base:instr.ip-base+instr.len]
        rawhex = " ".join(f"{b:02x}" for b in raw)
        text = fmt.format(instr)
        mark = ""
        if "+0x34]" in text:
            mark = "   <== ACCESO A +0x34"
        print(f"0x{rva:08X}  {rawhex:<28} {text}{mark}")
        if instr.mnemonic == Mnemonic.INT3:
            print("  (padding)"); break
        if stop_at_ret and instr.mnemonic == Mnemonic.RET:
            break

# 1) getter trivial: deberia ser mov eax,[rcx+0x34]; ret
disasm(0x289F20, 0x20, "getFundamentalNPCType (getter)", stop_at_ret=True)
# 2) ctor: buscar inicializacion de +0x34
disasm(0x800E00, 0x280, "Faction::Faction(ctor)")
# 3) setup: puede escribir el tipo desde GameData
disasm(0x7FD010, 0x200, "Faction::setup")
