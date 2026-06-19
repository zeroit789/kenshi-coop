# -*- coding: utf-8 -*-
# Cierre: identificar el objeto orden (vtable escrita), que devuelve 0x5E8410, y los lea de .rdata.
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

# 1) 0x1416852D0: que es? Es lo que se escribe en order.data[0] (vtable del struct orden temporal).
#    Y 0x1416F27B8 / 0x1416F2730 / 0x1416F2750 que usa 0x5E8410.
for addr in [0x16852D0, 0x16F27B8, 0x16F2730, 0x16F2750, 0x16F27B8-8]:
    rva = addr
    sec = which_sec(rva)
    name = read_rtti_name(rva)
    q0 = rd_qword(rva)
    print(f"  .data/.rdata 0x{rva:X} ({sec}) RTTI={name} slot0=0x{q0:X}" if q0 else f"  0x{rva:X} ({sec}) RTTI={name}")
    # si slot0 es codigo, resolver thunk y ver
    if q0:
        fn = q0 - IMAGE_BASE
        rt = resolve_thunk(fn)
        print(f"       slot0 -> RVA 0x{fn:X}" + (f" thunk=>0x{rt:X}" if rt else ""))

# 2) El objeto que devuelve 0x5E8410: en 0x674422 se hace mov r8,[rax]; call [r8] (vtable slot0).
#    rax = retorno de 0x5E8410 = [rbp+0x67]+0x38 (rama nueva) o rbx+0x38 (rama existente).
#    El objeto vive en rbx (un contenedor) +0x38. Veamos 0x5E6DE0 (lo que inserta el nuevo nodo)
#    y 0x5E7B20. El tipo del nodo = la vtable en 0x1416F27B8 (se escribe en [rbp-0x49]).
print("\n## Resolviendo el nodo creado por 0x5E8410 ##")
# El layout temporal en rbp escribe varias vtables; 0x1416F27B8 es la candidata principal (mov [rbp-0x49],rax).
for vt in [0x16F27B8, 0x16F2730, 0x16F2750]:
    nm = read_rtti_name(vt)
    print(f"  vtbl 0x{vt:X}: {nm}")
    # imprime primeros 4 slots resueltos
    for s in range(4):
        q = rd_qword(vt + s*8)
        if q:
            fn = q - IMAGE_BASE; rt = resolve_thunk(fn)
            print(f"     +0x{s*8:X}: RVA 0x{fn:X}" + (f" =>0x{rt:X}" if rt else ""))

# 3) Confirmar 0x673A90 (lo que llama 0x674300 en la rama tras encolar, modo!=4) brevemente
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
        if instr.flow_control in (FlowControl.CONDITIONAL_BRANCH, FlowControl.UNCONDITIONAL_BRANCH):
            try: mark = f"   ; -> 0x{instr.near_branch_target - IMAGE_BASE:X}"
            except Exception: pass
        if instr.mnemonic == Mnemonic.INT3: break
        print(f"0x{rva:08X}  {fmt.format(instr)}{mark}")

disasm(0x673A90, 0x60, "0x673A90 (insertar en lista, modo!=4)")
# 4) 0x798090 que usa setAttackTarget alterno 0x665650; y 0x5C9290 (notify target?)
disasm(0x5C9290, 0x50, "0x5C9290 (notify de setAttackTarget)")
# 5) resolver el lea 0x1416852D0 leyendo bytes crudos (puede ser una std::string vacia / vtable comun)
off = rva_to_off(0x16852D0)
print(f"\n  bytes en 0x16852D0 ({which_sec(0x16852D0)}): {DATA[off:off+32].hex(' ')}")
