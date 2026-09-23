# -*- coding: utf-8 -*-
# ES: Verificación del FIX CAUSA 2 (SetControlledChar + gate +0x250): imprime el prólogo de
#     SetControlledChar (0x802520), la desensambla marcando +0x250/+0x2A0/+0x2A8/+0x58 y desensambla la
#     rama viva 0x5CD1C0 para ver el gate exacto. Uso: python re_setcontrolledchar.py
# EN: Verification of FIX CAUSE 2 (SetControlledChar + +0x250 gate): prints the SetControlledChar prologue
#     (0x802520), disassembles it flagging +0x250/+0x2A0/+0x2A8/+0x58 and disassembles the live branch
#     0x5CD1C0 to see the exact gate. Usage: python re_setcontrolledchar.py

# RE de verificacion para el FIX CAUSA 2 (SetControlledChar + gate +0x250).
# Desensambla:
#   1) SetControlledChar 0x802520  -> firma, prologo, que escribe en +0x250 y +0x2A0
#   2) Rama viva 0x5CD1C0          -> el gate exacto que lee +0x250 (directo? o via vtbl+0x58?)
# EN: Verification RE for FIX CAUSE 2 (SetControlledChar + gate +0x250).
#     Disassembles:
#       1) SetControlledChar 0x802520  -> signature, prologue, what it writes into +0x250 and +0x2A0
#       2) Live branch 0x5CD1C0        -> the exact gate reading +0x250 (direct? or via vtbl+0x58?)
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
            if d < raw_size:
                return raw_off + d
    return None

# ES: Desensambla un rango desde start_rva e imprime cada instrucción con anotaciones (destinos, cadenas o marcas según el script).
# EN: Disassembles a range from start_rva and prints each instruction with annotations (targets, strings or marks depending on the script).
def disasm(start_rva, length, label, stop_on_ret=False):
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
        if instr.flow_control in (FlowControl.CONDITIONAL_BRANCH, FlowControl.UNCONDITIONAL_BRANCH):
            try:
                t = instr.near_branch_target - IMAGE_BASE
                mark = f"   ; -> 0x{t:X}"
            except Exception:
                pass
        s = fmt.format(instr)
        # resaltar accesos a 0x250 / 0x2A0 / 0x2A8 / 0x58
        # EN: highlight accesses to 0x250 / 0x2A0 / 0x2A8 / 0x58
        for needle in ("0x250", "0x2A0", "0x2A8", "0x58]", "+0x58"):
            if needle in s:
                mark += f"   <<< {needle}"
        print(f"0x{rva:08X}  {rawhex:<28} {s}{mark}")
        if instr.mnemonic == Mnemonic.INT3:
            print("   <-- PADDING (fin)"); break
        if stop_on_ret and instr.mnemonic == Mnemonic.RET:
            break

# Prologo de SetControlledChar (primeros bytes -> confirmar mov rax,rsp 48 8B C4)
# EN: SetControlledChar prologue (first bytes -> confirm mov rax,rsp 48 8B C4)
off = rva_to_off(0x802520)
print("PROLOGO BYTES SetControlledChar 0x802520:", " ".join(f"{b:02x}" for b in DATA[off:off+16]))

disasm(0x802520, 0x180, "SetControlledChar 0x802520 (firma/prologo/escrituras +0x250 y +0x2A0)")
disasm(0x5CD1C0, 0x70, "Rama viva 0x5CD1C0 (gate del think pesado +0x250 / +0xD8)")
