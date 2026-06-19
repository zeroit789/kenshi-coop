# -*- coding: utf-8 -*-
# Verifica: (1) resto de 0x86DB80 tras el setFaction, (2) que 0x802520 es player->setFaction
# (escribe la faccion en algun offset del player), (3) que 0x2E77F0 resuelve Faction por StringId,
# (4) quien llama a 0x86DB80 (xrefs call), (5) que 0x2134130 es el contenedor de items (findById).
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register, FlowControl

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
def get_text():
    for n,s,v,ro,rs in SECTIONS:
        if n==".text": return s,ro,rs
def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size: return raw_off + d
    return None
def read_cstr(va, maxlen=80):
    rva = va - IMAGE_BASE; off = rva_to_off(rva)
    if off is None: return None
    end = DATA.find(b"\x00", off, off+maxlen)
    if end == -1: return None
    try:
        s = DATA[off:end].decode("ascii")
        if all(32 <= ord(c) < 127 for c in s) and len(s) >= 2: return s
    except: return None
def resolve_thunk(va):
    off = rva_to_off(va-IMAGE_BASE)
    if off is None: return None
    ins = next(iter(Decoder(64, DATA[off:off+16], ip=va)), None)
    if ins and ins.mnemonic==Mnemonic.JMP and ins.op0_kind==OpKind.NEAR_BRANCH64:
        return ins.near_branch_target
    return None
def disasm(start_rva, length, label, maxi=120):
    off = rva_to_off(start_rva)
    dec = Decoder(64, DATA[off:off+length], ip=IMAGE_BASE+start_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n===== {label} (0x{start_rva:X}) =====")
    c=0
    for instr in dec:
        c+=1
        if c>maxi: break
        rva=instr.ip-IMAGE_BASE; txt=fmt.format(instr); note=""
        if instr.memory_base==Register.RIP:
            s=read_cstr(instr.memory_displacement)
            note = f'   ; "{s}"' if s else f"   ; [abs 0x{instr.memory_displacement-IMAGE_BASE:X}]"
        if instr.flow_control==FlowControl.CALL and instr.op0_kind==OpKind.NEAR_BRANCH64:
            t=instr.near_branch_target; th=resolve_thunk(t)
            note=f"   ; -> 0x{th-IMAGE_BASE:X}" if th else f"   ; -> sub_0x{t-IMAGE_BASE:X}"
        print(f"0x{rva:08X}  {txt}{note}")

# (1) resto de 0x86DB80 tras setFaction
disasm(0x86DDD6, 0x60, "RESTO 0x86DB80 tras setFaction(player,Nameless)")
# (2) 0x802520 supuesto player->setFaction
disasm(0x802520, 0x90, "0x802520 (supuesto Character/Player::setFaction)")
# (3) 0x2E77F0 lookup faction por StringId
disasm(0x2E77F0, 0x80, "0x2E77F0 (lookup Faction por StringId)")

# (4) callers de 0x86DB80: escanear .text por call/jmp directos a 0x86DB80 (y a su thunk si existe)
s,ro,rs = get_text()
targets = {0x86DB80}
# buscar thunk jmp->0x86DB80
dec = Decoder(64, DATA[ro:ro+rs], ip=IMAGE_BASE+s)
thunks=set()
for instr in dec:
    if instr.mnemonic==Mnemonic.JMP and instr.op0_kind==OpKind.NEAR_BRANCH64 and (instr.near_branch_target-IMAGE_BASE)==0x86DB80:
        thunks.add(instr.ip-IMAGE_BASE)
targetsVA = {IMAGE_BASE+0x86DB80} | {IMAGE_BASE+t for t in thunks}
print(f"\n===== CALLERS de 0x86DB80 (thunks={[hex(t) for t in thunks]}) =====")
dec = Decoder(64, DATA[ro:ro+rs], ip=IMAGE_BASE+s)
for instr in dec:
    if instr.flow_control in (FlowControl.CALL,) and instr.op0_kind==OpKind.NEAR_BRANCH64:
        if instr.near_branch_target in targetsVA:
            print(f"  call desde RVA 0x{instr.ip-IMAGE_BASE:08X} -> 0x{instr.near_branch_target-IMAGE_BASE:X}")
