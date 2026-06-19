# -*- coding: utf-8 -*-
# RE para el fix de relaciones de faccion (Kenshi Co-op).
# Resuelve: prologos+AOB de isEnemy/addRelation/setter, desensamblado del setter reciproco,
# layout de FactionManager (array de Faction*), y getter por string-id/nombre.
import struct, sys
from iced_x86 import (Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind,
                      Register, FlowControl, OpCodeOperandKind)

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

def sec_of_rva(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            return name
    return None

def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size:
                return raw_off + d
    return None

TEXT = next(s for s in SECTIONS if s[0] == ".text")
RDATA = next(s for s in SECTIONS if s[0] == ".rdata")
DATAS = next(s for s in SECTIONS if s[0] == ".data")

def hexbytes(rva, n):
    off = rva_to_off(rva)
    return " ".join(f"{b:02X}" for b in DATA[off:off+n])

def disasm(start_rva, length, label, stop_at_int3=False, max_ins=999):
    off = rva_to_off(start_rva)
    code = DATA[off:off+length]
    dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} @ RVA 0x{start_rva:X} ===")
    n = 0
    for instr in dec:
        if n >= max_ins: break
        n += 1
        rva = instr.ip - IMAGE_BASE
        base_off = instr.ip-(IMAGE_BASE+start_rva)
        raw = code[base_off:base_off+instr.len]
        rawhex = " ".join(f"{b:02x}" for b in raw)
        mark = ""
        if instr.flow_control in (FlowControl.CONDITIONAL_BRANCH, FlowControl.UNCONDITIONAL_BRANCH,
                                  FlowControl.CALL, FlowControl.INDIRECT_CALL):
            try:
                t = instr.near_branch_target - IMAGE_BASE
                if t: mark = f"   ; -> 0x{t:X}"
            except Exception:
                pass
        if instr.mnemonic == Mnemonic.INT3 and stop_at_int3:
            print(f"0x{rva:08X}  {rawhex:<28} {fmt.format(instr)}  <-- PADDING"); break
        print(f"0x{rva:08X}  {rawhex:<28} {fmt.format(instr)}{mark}")

if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "prologues"

    if what == "prologues":
        for name, rva in [("isEnemy", 0x6B26D0), ("isAlly", 0x6B2630),
                          ("addRelation", 0x6B2EA0), ("setterReciproco", 0x39F660),
                          ("ctorFaction", 0x802070), ("getRelationEntry", 0x6B4C60)]:
            print(f"\n--- {name} 0x{rva:X} primeros 24 bytes: {hexbytes(rva,24)}")

    if what == "isenemy":
        disasm(0x6B26D0, 0x80, "isEnemy", stop_at_int3=True)
    if what == "isally":
        disasm(0x6B2630, 0x80, "isAlly", stop_at_int3=True)
    if what == "addrel":
        disasm(0x6B2EA0, 0xC0, "addRelation", stop_at_int3=True)
    if what == "setter":
        disasm(0x39F660, 0x140, "setterReciproco", stop_at_int3=True)
    if what == "ctor":
        disasm(0x802070, 0x140, "ctorFaction", stop_at_int3=True)
    if what == "getentry":
        disasm(0x6B4C60, 0x100, "getRelationEntry", stop_at_int3=True)
