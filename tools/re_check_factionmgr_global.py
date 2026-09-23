# -*- coding: utf-8 -*-
# ES: Verifica las globales FactionManager (RVA 0x21345B8) y GameWorld (RVA 0x2134110): busca en .text
#     instrucciones RIP-relativas comunes (prefijo 48/4C + mov/lea/cmp/add) que apunten a ellas y muestra
#     el contexto desensamblado de las primeras 14. Uso: python re_check_factionmgr_global.py
# EN: Verifies the FactionManager (RVA 0x21345B8) and GameWorld (RVA 0x2134110) globals: searches .text
#     for common RIP-relative instructions (48/4C prefix + mov/lea/cmp/add) pointing to them and shows the
#     disassembled context of the first 14. Usage: python re_check_factionmgr_global.py

# Verifica el FactionManager global modBase+0x21345B8 y el GameWorld modBase+0x2134110:
# busca xrefs RIP-rel a esas direcciones absolutas y muestra contexto desensamblado.
# EN: Verifies the global FactionManager modBase+0x21345B8 and GameWorld modBase+0x2134110:
#     searches RIP-rel xrefs to those absolute addresses and shows disassembled context.
import struct, sys
from iced_x86 import (Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl)

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
DATA = open(EXE, "rb").read()
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
TEXT = next(s for s in SECTIONS if s[0]==".text")

# ES: RVA -> offset en el fichero (None si el RVA no tiene datos en disco).
# EN: RVA -> file offset (None if the RVA has no on-disk data).
def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size: return raw_off + d
    return None
# ES: Offset de fichero -> RVA.
# EN: File offset -> RVA.
def off_to_rva(off):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if raw_off <= off < raw_off + raw_size:
            return off - raw_off + srva
    return None
# ES: Nombre de la sección que contiene el RVA.
# EN: Name of the section containing the RVA.
def sec_of(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size): return name
    return None

# ES: Lista (RVA, opcode) de las instrucciones RIP-relativas 48/4C cuyo destino es target_rva.
# EN: List of (RVA, opcode) of the 48/4C RIP-relative instructions whose target is target_rva.
def xrefs_to_abs(target_rva):
    """RIP-rel (lea/mov/cmp/etc, opcode generico 48/4C ..) a la direccion absoluta target."""
    t0=TEXT[3]; tsz=TEXT[4]
    out=[]
    d=DATA
    for i in range(t0, t0+tsz-8):
        b0=d[i]
        # ES: (bloque sin efecto: bucle vacío)
        # EN: (no-op block: empty loop)
        if b0 in (0x48,0x4C,0x44,0x66):
            for oplen_b1 in range(1,3):
                pass
        # ES: decodificar RIP-rel mod=00 rm=101 en instrucciones que empiezan por 48/4C con opcodes comunes
        # decode RIP-rel mod=00 rm=101 in instructions starting 48/4C with common opcodes
        if b0 in (0x48,0x4C):
            b1=d[i+1]
            if b1 in (0x8B,0x8D,0x89,0x39,0x3B,0x01,0x03):
                modrm=d[i+2]; mod=(modrm>>6)&3; rm=modrm&7
                if mod==0 and rm==5:
                    disp=struct.unpack_from("<i",d,i+3)[0]
                    ir=off_to_rva(i)
                    if ir is None: continue
                    if ir+7+disp==target_rva:
                        out.append((ir,b0,b1))
    return out

# ES: Desensambla un rango desde start_rva e imprime cada instrucción con anotaciones (destinos, cadenas o marcas según el script).
# EN: Disassembles a range from start_rva and prints each instruction with annotations (targets, strings or marks depending on the script).
def disasm(start_rva, length, max_ins=40):
    off=rva_to_off(start_rva)
    code=DATA[off:off+length]
    dec=Decoder(64,code,ip=IMAGE_BASE+start_rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    n=0
    for instr in dec:
        if n>=max_ins: break
        n+=1
        rva=instr.ip-IMAGE_BASE
        bo=instr.ip-(IMAGE_BASE+start_rva)
        raw=code[bo:bo+instr.len]
        rh=" ".join(f"{b:02x}" for b in raw)
        mark=""
        if instr.flow_control==FlowControl.CALL:
            try:
                t=instr.near_branch_target-IMAGE_BASE
                if t: mark=f"   ; call 0x{t:X}"
            except: pass
        print(f"  0x{rva:08X}  {rh:<24} {fmt.format(instr)}{mark}")

# ES: Punto de entrada: xrefs de las dos globales y su contexto.
# EN: Entry point: xrefs of both globals and their context.
if __name__=="__main__":
    targets = {
        "FactionManager_global(modBase+0x21345B8)": 0x21345B8,
        "GameWorld_embed(modBase+0x2134110)": 0x2134110,
    }
    for label,tr in targets.items():
        xs=xrefs_to_abs(tr)
        print(f"\n############ {label} = 0x{tr+IMAGE_BASE:X} : {len(xs)} xrefs ############")
        for ir,b0,b1 in xs[:14]:
            print(f"\n  -- xref @ 0x{ir:X} (opcode {b0:02x} {b1:02x}) contexto:")
            disasm(ir-7, 0x48, max_ins=12)
