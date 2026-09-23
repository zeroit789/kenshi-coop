# -*- coding: utf-8 -*-
# ES: Cierre del flujo melee: identifica el objeto "orden" (vtable escrita), lo que devuelve la factory
#     0x5E8410 y los lea de .rdata: RTTI y primeros slots de 0x16852D0 / 0x16F27B8 / 0x16F2730 /
#     0x16F2750, y desensambla 0x673A90 y 0x5C9290. Uso: python re_melee_flow6.py
# EN: Melee-flow wrap-up: identifies the "order" object (written vtable), what factory 0x5E8410 returns
#     and the .rdata leas: RTTI and first slots of 0x16852D0 / 0x16F27B8 / 0x16F2730 / 0x16F2750, and
#     disassembles 0x673A90 and 0x5C9290. Usage: python re_melee_flow6.py

# Cierre: identificar el objeto orden (vtable escrita), que devuelve 0x5E8410, y los lea de .rdata.
# EN: Wrap-up: identify the order object (written vtable), what 0x5E8410 returns, and the .rdata leas.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
with open(EXE, "rb") as f: DATA = f.read()
# ES: Tabla de secciones leída a mano de las cabeceras PE (e_lfanew -> COFF -> cabeceras de sección de 40 bytes).
# EN: Section table parsed by hand from the PE headers (e_lfanew -> COFF -> 40-byte section headers).
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
# ES: RVA -> offset en el fichero (None si el RVA no tiene datos en disco).
# EN: RVA -> file offset (None if the RVA has no on-disk data).
def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size: return raw_off + d
    return None
# ES: Nombre de la sección que contiene el RVA.
# EN: Name of the section containing the RVA.
def which_sec(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size): return name
    return "FUERA"
# ES: Lee un qword (8 bytes) en un RVA.
# EN: Reads a qword (8 bytes) at an RVA.
def rd_qword(rva):
    off = rva_to_off(rva); return struct.unpack_from("<Q", DATA, off)[0] if off is not None else None
# ES: Lee un dword (4 bytes) en un RVA.
# EN: Reads a dword (4 bytes) at an RVA.
def rd_dword(rva):
    off = rva_to_off(rva); return struct.unpack_from("<I", DATA, off)[0] if off is not None else None
# ES: Si en la dirección hay un JMP (thunk), devuelve su destino real (según la variante sigue uno o varios saltos).
# EN: If there is a JMP (thunk) at the address, returns its real target (one or several hops depending on the variant).
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
# ES: Nombre RTTI de la vtable (COL en vt-8 -> TD -> nombre), o None.
# EN: RTTI name of the vtable (COL at vt-8 -> TD -> name), or None.
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
# EN: 1) 0x1416852D0: what is it? It is what gets written into order.data[0] (vtable of the temporary order struct).
#        And 0x1416F27B8 / 0x1416F2730 / 0x1416F2750 used by 0x5E8410.
for addr in [0x16852D0, 0x16F27B8, 0x16F2730, 0x16F2750, 0x16F27B8-8]:
    rva = addr
    sec = which_sec(rva)
    name = read_rtti_name(rva)
    q0 = rd_qword(rva)
    print(f"  .data/.rdata 0x{rva:X} ({sec}) RTTI={name} slot0=0x{q0:X}" if q0 else f"  0x{rva:X} ({sec}) RTTI={name}")
    # si slot0 es codigo, resolver thunk y ver
    # EN: if slot0 is code, resolve the thunk and look
    if q0:
        fn = q0 - IMAGE_BASE
        rt = resolve_thunk(fn)
        print(f"       slot0 -> RVA 0x{fn:X}" + (f" thunk=>0x{rt:X}" if rt else ""))

# 2) El objeto que devuelve 0x5E8410: en 0x674422 se hace mov r8,[rax]; call [r8] (vtable slot0).
#    rax = retorno de 0x5E8410 = [rbp+0x67]+0x38 (rama nueva) o rbx+0x38 (rama existente).
#    El objeto vive en rbx (un contenedor) +0x38. Veamos 0x5E6DE0 (lo que inserta el nuevo nodo)
#    y 0x5E7B20. El tipo del nodo = la vtable en 0x1416F27B8 (se escribe en [rbp-0x49]).
# EN: 2) The object returned by 0x5E8410: at 0x674422 it does mov r8,[rax]; call [r8] (vtable slot0).
#        rax = return of 0x5E8410 = [rbp+0x67]+0x38 (new branch) or rbx+0x38 (existing branch).
#        The object lives in rbx (a container) +0x38. Let us look at 0x5E6DE0 (inserts the new node)
#        and 0x5E7B20. The node type = the vtable at 0x1416F27B8 (written to [rbp-0x49]).
print("\n## Resolviendo el nodo creado por 0x5E8410 ##")
# El layout temporal en rbp escribe varias vtables; 0x1416F27B8 es la candidata principal (mov [rbp-0x49],rax).
# EN: The temporary layout in rbp writes several vtables; 0x1416F27B8 is the main candidate (mov [rbp-0x49],rax).
for vt in [0x16F27B8, 0x16F2730, 0x16F2750]:
    nm = read_rtti_name(vt)
    print(f"  vtbl 0x{vt:X}: {nm}")
    # imprime primeros 4 slots resueltos
    # EN: print the first 4 resolved slots
    for s in range(4):
        q = rd_qword(vt + s*8)
        if q:
            fn = q - IMAGE_BASE; rt = resolve_thunk(fn)
            print(f"     +0x{s*8:X}: RVA 0x{fn:X}" + (f" =>0x{rt:X}" if rt else ""))

# 3) Confirmar 0x673A90 (lo que llama 0x674300 en la rama tras encolar, modo!=4) brevemente
# ES: Desensambla un rango desde start_rva e imprime cada instrucción con anotaciones (destinos, cadenas o marcas según el script).
# EN: Disassembles a range from start_rva and prints each instruction with annotations (targets, strings or marks depending on the script).
# EN: 3) Briefly confirm 0x673A90 (what 0x674300 calls in the branch after enqueuing, mode!=4)
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
# EN: 4) 0x798090 used by the alternate setAttackTarget 0x665650; and 0x5C9290 (notify target?)
disasm(0x5C9290, 0x50, "0x5C9290 (notify de setAttackTarget)")
# 5) resolver el lea 0x1416852D0 leyendo bytes crudos (puede ser una std::string vacia / vtable comun)
# EN: 5) resolve lea 0x1416852D0 by reading raw bytes (may be an empty std::string / common vtable)
off = rva_to_off(0x16852D0)
print(f"\n  bytes en 0x16852D0 ({which_sec(0x16852D0)}): {DATA[off:off+32].hex(' ')}")
