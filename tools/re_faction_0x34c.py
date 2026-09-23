# -*- coding: utf-8 -*-
# ES: Paso final de Faction+0x34: desensambla los 4 getters candidatos y busca en .text escrituras
#     "mov dword [reg+0x34], imm" con inmediatos 0..10 (valores del enum) y una muestra de
#     "mov [reg+0x34], reg32". Uso: python re_faction_0x34c.py
# EN: Final Faction+0x34 step: disassembles the 4 candidate getters and searches .text for
#     "mov dword [reg+0x34], imm" writes with immediates 0..10 (enum values) and a sample of
#     "mov [reg+0x34], reg32". Usage: python re_faction_0x34c.py

# Paso final: (1) desensamblar los 4 getters candidatos +0x34.
# (2) Buscar ESCRITURAS de un inmediato en [reg+0x34] (mov dword[reg+0x34], imm32)
#     en TODO .text, filtrando por inmediatos del enum 0..10. Reportar contexto.
# EN: Final step: (1) disassemble the 4 candidate +0x34 getters.
#     (2) Find WRITES of an immediate into [reg+0x34] (mov dword[reg+0x34], imm32)
#         across ALL of .text, filtering by enum immediates 0..10. Report context.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register

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
# ES: (RVA, offset, tamaño) de .text.
# EN: (RVA, offset, size) of .text.
def text_seg():
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if name==".text": return srva, raw_off, raw_size
fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""

# ES: Desensambla un rango desde start_rva e imprime cada instrucción con anotaciones (destinos, cadenas o marcas según el script).
# EN: Disassembles a range from start_rva and prints each instruction with annotations (targets, strings or marks depending on the script).
def disasm(start_rva, length, label):
    off = rva_to_off(start_rva)
    code = DATA[off:off+length]
    dec = Decoder(64, code, ip=IMAGE_BASE+start_rva)
    print(f"\n=== {label} 0x{start_rva:X} ===")
    base=IMAGE_BASE+start_rva
    for instr in dec:
        rva=instr.ip-IMAGE_BASE
        raw=code[instr.ip-base:instr.ip-base+instr.len]
        rawhex=" ".join(f"{b:02x}" for b in raw)
        print(f"0x{rva:08X}  {rawhex:<20} {fmt.format(instr)}")
        if instr.mnemonic in (Mnemonic.RET, Mnemonic.INT3): break

# ES: Getters candidatos.
# EN: Candidate getters.
for g in (0x28A3D0, 0xAAF4A0, 0xADB270, 0xB8F120):
    disasm(g, 0x10, "getter +0x34")

# Escaneo de ESCRITURAS inmediatas a [reg+0x34] con disp8 = 0x34.
# Patron C7 /0 ib? -> mov r/m32, imm32 con modrm disp8.
# Cubrimos modrm bytes con disp8: opcode C7, modrm = 01 xxx 000..111 donde disp8 sigue.
# Mejor: decodificar linealmente .text es caro; en su lugar buscamos byte-pattern del disp 0x34
# en instrucciones 'mov dword ptr [reg+0x34], imm' = C7 4X 34 II 00 00 00  (X reg, II=imm low).
# EN: Scan for immediate WRITES to [reg+0x34] with disp8 = 0x34.
#     Pattern C7 /0 ib? -> mov r/m32, imm32 with modrm disp8.
#     We cover modrm bytes with disp8: opcode C7, modrm = 01 xxx 000..111 followed by disp8.
#     Better: linearly decoding .text is expensive; instead we look for the byte pattern of disp 0x34
#     in 'mov dword ptr [reg+0x34], imm' instructions = C7 4X 34 II 00 00 00  (X reg, II=imm low).
print("\n=== Escrituras 'mov dword[reg+0x34], imm (0..10)' (C7 4? 34 0?..0A 00 00 00) ===")
srva, raw_off, raw_size = text_seg()
seg = DATA[raw_off:raw_off+raw_size]
i=0; hits=0
while i < len(seg)-7:
    if seg[i]==0xC7 and (seg[i+1] & 0xF8)==0x40 and seg[i+2]==0x34:
        imm = struct.unpack_from("<I", seg, i+3)[0]
        if imm <= 10:
            rva = srva+i
            reg = seg[i+1] & 0x07
            regn = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi"][reg]
            print(f"  RVA 0x{rva:08X}  mov dword[{regn}+0x34], {imm}")
            hits+=1
    i+=1
print(f"  total: {hits}")

# Tambien escrituras con registro a [rsi/rdi/rcx/rbx+0x34]: 89 4? 34 (mov [reg+0x34], reg32)
# EN: Also register writes to [rsi/rdi/rcx/rbx+0x34]: 89 4? 34 (mov [reg+0x34], reg32)
print("\n=== Escrituras 'mov [reg+0x34], reg32' (89 4? 34) -- muestreo ===")
i=0; hits=0
while i < len(seg)-3:
    if seg[i]==0x89 and (seg[i+1] & 0xC0)==0x40 and seg[i+2]==0x34:
        rva=srva+i
        # decodificar la instruccion en contexto para texto fiable
        # EN: decode the instruction in context for reliable text
        dec=Decoder(64, seg[i:i+8], ip=IMAGE_BASE+rva)
        ins=next(iter(dec))
        print(f"  RVA 0x{rva:08X}  {fmt.format(ins)}")
        hits+=1
        if hits>30: print("  ...mas"); break
    i+=1
print(f"  total mostrado: {hits}")
