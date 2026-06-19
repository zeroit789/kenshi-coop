# -*- coding: utf-8 -*-
# Final: que vtable lleva el nodo orden real. 0x5E8410 devuelve [rbp+0x67]+0x38 o rbx+0x38.
# El nodo se crea en 0x5E6DE0 (inserta par clave/valor en un map). El valor (offset +0x38 del nodo)
# es un objeto con vtable en [valor]. Buscamos donde se escribe esa vtable: en 0x5E6DE0 o en el
# constructor del value_type. Tambien: 0x671840 (lo que llaman 0x673A90/0x674300 con &order).
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
with open(EXE, "rb") as f: DATA = f.read()
e_lfanew = struct.unpack_from("<I", DATA, 0x3C)[0]; coff = e_lfanew + 4
num_sec = struct.unpack_from("<H", DATA, coff + 2)[0]
opt_size = struct.unpack_from("<H", DATA, coff + 16)[0]
sec_off = coff + 20 + opt_size
SECTIONS = []
for i in range(num_sec):
    o = sec_off + i*40
    name = DATA[o:o+8].rstrip(b"\x00").decode("ascii","ignore")
    vsize = struct.unpack_from("<I", DATA, o+8)[0]; rva = struct.unpack_from("<I", DATA, o+12)[0]
    raw_size = struct.unpack_from("<I", DATA, o+16)[0]; raw_off = struct.unpack_from("<I", DATA, o+20)[0]
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
    return "FUERA"
def rd_qword(rva):
    off = rva_to_off(rva); return struct.unpack_from("<Q", DATA, off)[0] if off is not None else None
def rd_dword(rva):
    off = rva_to_off(rva); return struct.unpack_from("<I", DATA, off)[0] if off is not None else None
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
def disasm(start_rva, length, label):
    off = rva_to_off(start_rva)
    if off is None: print(f"\n=== {label} 0x{start_rva:X} sin raw ==="); return
    code = DATA[off:off+length]; dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} 0x{start_rva:X} ({which_sec(start_rva)}) ===")
    for instr in dec:
        rva = instr.ip - IMAGE_BASE
        mark = ""
        if instr.flow_control == FlowControl.CALL:
            try:
                tt = instr.near_branch_target - IMAGE_BASE; rt = resolve_thunk(tt)
                mark = f"   ; CALL 0x{tt:X}" + (f" =>0x{rt:X}" if rt else "")
            except Exception: pass
        elif instr.flow_control in (FlowControl.CONDITIONAL_BRANCH, FlowControl.UNCONDITIONAL_BRANCH):
            try: mark = f"   ; -> 0x{instr.near_branch_target - IMAGE_BASE:X}"
            except Exception: pass
        # marcar lea de vtable rdata con RTTI
        if instr.mnemonic == Mnemonic.LEA:
            try:
                t = instr.memory_displacement - IMAGE_BASE
                nm = read_rtti_name(t)
                if nm: mark += f"   ; VTBL {nm}"
            except Exception: pass
        if instr.mnemonic == Mnemonic.INT3: break
        print(f"0x{rva:08X}  {fmt.format(instr)}{mark}")

# 0x5E6DE0: inserta el value en el map; aqui se construye el nodo orden con su vtable
disasm(0x5E6DE0, 0x100, "0x5E6DE0 (construye/inserta nodo orden -> vtable del objeto)")
# 0x671840: lo llaman 0x673A90 y 0x674300 con (this=char, &order); puede ser el verdadero addOrder
disasm(0x671840, 0xA0, "0x671840 (addOrder real? llamado con &order)")
