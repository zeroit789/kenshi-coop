# -*- coding: utf-8 -*-
# Desensamblador generico con resolucion de strings RIP-rel y llamadas. Uso:
#   python re_disasm_range.py 0x86DB80 0x260
import struct, sys
from iced_x86 import (Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl)

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
DATA = open(EXE, "rb").read()
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
            if d < raw_size: return raw_off + d
    return None
def sec_of(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size): return name
    return None
def read_cstr(off, maxlen=80):
    e = DATA.find(b"\x00", off, off+maxlen)
    if e==-1: e=off+maxlen
    return DATA[off:e].decode("ascii","ignore")

def disasm(start_rva, length, max_ins=400):
    off=rva_to_off(start_rva)
    code=DATA[off:off+length]
    dec=Decoder(64,code,ip=IMAGE_BASE+start_rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"=== disasm @ RVA 0x{start_rva:X} (len 0x{length:X}) ===")
    n=0
    for instr in dec:
        if n>=max_ins: break
        n+=1
        rva=instr.ip-IMAGE_BASE
        bo=instr.ip-(IMAGE_BASE+start_rva)
        raw=code[bo:bo+instr.len]
        rh=" ".join(f"{b:02x}" for b in raw)
        mark=""
        if instr.flow_control in (FlowControl.CONDITIONAL_BRANCH,FlowControl.UNCONDITIONAL_BRANCH,FlowControl.CALL):
            try:
                t=instr.near_branch_target-IMAGE_BASE
                if t: mark=f"   ; -> 0x{t:X} ({sec_of(t)})"
            except: pass
        try:
            if instr.is_ip_rel_memory_operand:
                tgt = instr.ip_rel_memory_address - IMAGE_BASE
                s2 = sec_of(tgt)
                if s2 in (".rdata",".data"):
                    o2 = rva_to_off(tgt)
                    if o2 is not None:
                        s = read_cstr(o2)
                        if s and all(32<=ord(c)<127 for c in s) and len(s)>=2:
                            mark += f'   ; "{s}"'
                        else:
                            mark += f'   ; [{s2}:0x{tgt:X}]'
        except Exception:
            pass
        tag = ""
        if instr.mnemonic==Mnemonic.INT3:
            tag=" <--PAD"
        print(f"0x{rva:08X}  {rh:<28} {fmt.format(instr)}{mark}{tag}")
        if instr.mnemonic==Mnemonic.INT3:
            break

if __name__=="__main__":
    rva=int(sys.argv[1],16)
    ln=int(sys.argv[2],16) if len(sys.argv)>2 else 0x200
    disasm(rva,ln)
