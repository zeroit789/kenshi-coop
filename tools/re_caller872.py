# -*- coding: utf-8 -*-
# ES: Contexto del llamador 0x872DBB (que llama a 0x86DB80, arranque de la facción Nameless): localiza su
#     función vía .pdata y lista las cadenas que referencia para identificarla (gamestart/newgame/load).
#     Uso: python re_caller872.py
# EN: Context of caller 0x872DBB (which calls 0x86DB80, the Nameless faction bootstrap): finds its
#     function via .pdata and lists the strings it references to identify it (gamestart/newgame/load).
#     Usage: python re_caller872.py

# Contexto del caller 0x872DBB (que llama a 0x86DB80). Identifica la funcion contenedora
# via .pdata y busca strings de debug en ella (gamestart/newgame/load). Tambien resuelve
# 0x2134130 (contenedor del findById de los 3 primeros xrefs): mira su uso.
# EN: Context of caller 0x872DBB (which calls 0x86DB80). Identifies the containing function
#     via .pdata and looks for debug strings in it (gamestart/newgame/load). It also resolves
#     0x2134130 (container of the findById of the first 3 xrefs): looks at its use (not implemented).
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register, FlowControl

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
            if d < raw_size: return raw_off + d
    return None
# ES: Lee una cadena ASCII terminada en 0 (devuelve None si no parece texto imprimible, según la variante).
# EN: Reads a NUL-terminated ASCII string (returns None if it does not look printable, depending on the variant).
def read_cstr(va, maxlen=90):
    rva = va - IMAGE_BASE; off = rva_to_off(rva)
    if off is None: return None
    end = DATA.find(b"\x00", off, off+maxlen)
    if end == -1: return None
    try:
        s = DATA[off:end].decode("ascii")
        if all(32 <= ord(c) < 127 for c in s) and len(s) >= 2: return s
    except: return None
# ES: Si en la dirección hay un JMP (thunk), devuelve su destino real (según la variante sigue uno o varios saltos).
# EN: If there is a JMP (thunk) at the address, returns its real target (one or several hops depending on the variant).
def resolve_thunk(va):
    off = rva_to_off(va-IMAGE_BASE)
    if off is None: return None
    ins = next(iter(Decoder(64, DATA[off:off+16], ip=va)), None)
    if ins and ins.mnemonic==Mnemonic.JMP and ins.op0_kind==OpKind.NEAR_BRANCH64:
        return ins.near_branch_target
    return None
# .pdata
# EN: .pdata
def load_pdata():
    for name, srva2, vsize, ro, rs in SECTIONS:
        if name==".pdata": return srva2, ro, rs
psrva,pro,prs = load_pdata()
# ES: Carga la tabla RUNTIME_FUNCTION (inicio, fin) ordenada.
# EN: Loads the RUNTIME_FUNCTION table (begin, end), sorted.
funcs=[]
for k in range(prs//12):
    b=pro+k*12
    beg=struct.unpack_from("<I",DATA,b)[0]; end=struct.unpack_from("<I",DATA,b+4)[0]
    if beg or end: funcs.append((beg,end))
funcs.sort()
# ES: Búsqueda binaria en .pdata: (inicio, fin) de la función que contiene rva, o None.
# EN: Binary search in .pdata: (begin, end) of the function containing rva, or None.
def func_of(rva):
    lo,hi=0,len(funcs)-1
    while lo<=hi:
        m=(lo+hi)//2; b,e=funcs[m]
        if b<=rva<e: return b,e
        if rva<b: hi=m-1
        else: lo=m+1
    return None

fo = func_of(0x872DBB)
print(f"caller 0x872DBB -> func {hex(fo[0])}..{hex(fo[1])} (off +0x{0x872DBB-fo[0]:X})" if fo else "no func")

# ES: Desensambla un rango desde start_rva e imprime cada instrucción con anotaciones (destinos, cadenas o marcas según el script).
# EN: Disassembles a range from start_rva and prints each instruction with annotations (targets, strings or marks depending on the script).
def disasm(start_rva, end_rva, label, maxi=600):
    off=rva_to_off(start_rva); dec=Decoder(64,DATA[off:off+(end_rva-start_rva)],ip=IMAGE_BASE+start_rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n===== {label} =====")
    c=0
    for instr in dec:
        c+=1
        if c>maxi: print("...trunc"); break
        rva=instr.ip-IMAGE_BASE; txt=fmt.format(instr); note=""
        if instr.memory_base==Register.RIP:
            s=read_cstr(instr.memory_displacement)
            note=f'   ; "{s}"' if s else f"   ; [abs 0x{instr.memory_displacement-IMAGE_BASE:X}]"
        if instr.flow_control==FlowControl.CALL and instr.op0_kind==OpKind.NEAR_BRANCH64:
            t=instr.near_branch_target; th=resolve_thunk(t)
            note=f"   ; -> 0x{th-IMAGE_BASE:X}" if th else f"   ; -> sub_0x{t-IMAGE_BASE:X}"
        # marcar solo lineas con string o call para reducir ruido
        # EN: only mark lines with a string or call to reduce noise (in practice every line is printed)
        print(f"0x{rva:08X}  {txt}{note}")

# Volcar solo las lineas con strings/calls de la funcion del caller para identificarla
# EN: Dump only the lines with strings/calls of the caller function to identify it
if fo:
    # imprimir strings de debug de la funcion completa
    # EN: print the debug strings of the whole function
    off=rva_to_off(fo[0]); dec=Decoder(64,DATA[off:off+(fo[1]-fo[0])],ip=IMAGE_BASE+fo[0])
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n===== STRINGS/CALLS en func caller {hex(fo[0])} =====")
    for instr in dec:
        rva=instr.ip-IMAGE_BASE
        if instr.memory_base==Register.RIP:
            s=read_cstr(instr.memory_displacement)
            if s: print(f"  0x{rva:08X}  STR \"{s}\"")
