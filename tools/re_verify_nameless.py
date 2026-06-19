# -*- coding: utf-8 -*-
# Verifica en BYTES los offsets para localizar la faccion "Nameless" via FactionManager.
# Desensambla: Faction::getName, Faction::getData, FactionManager::getFactionByStringID,
# getFactionByName, getAllFactions, y RootObjectBase::getGameData.
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
def off_to_rva(off):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if raw_off <= off < raw_off + raw_size:
            return off - raw_off + srva
    return None
def sec_of(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size): return name
    return None

def read_cstr(off, maxlen=64):
    e = DATA.find(b"\x00", off, off+maxlen)
    if e==-1: e=off+maxlen
    return DATA[off:e].decode("ascii","ignore")

def disasm(start_rva, length, label, stop_int3=True, max_ins=120):
    off=rva_to_off(start_rva)
    code=DATA[off:off+length]
    dec=Decoder(64,code,ip=IMAGE_BASE+start_rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    print(f"\n=== {label} @ RVA 0x{start_rva:X} ===")
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
        # Resolver RIP-relativo a strings en .rdata
        try:
            if instr.is_ip_rel_memory_operand:
                tgt = instr.ip_rel_memory_address - IMAGE_BASE
                if sec_of(tgt) in (".rdata",".data"):
                    o2 = rva_to_off(tgt)
                    if o2 is not None:
                        s = read_cstr(o2)
                        if s and all(32<=ord(c)<127 for c in s) and len(s)>=2:
                            mark += f'   ; data="{s}"'
        except Exception:
            pass
        if instr.mnemonic==Mnemonic.INT3 and stop_int3:
            print(f"0x{rva:08X}  {rh:<26} {fmt.format(instr)} <--PAD"); break
        print(f"0x{rva:08X}  {rh:<26} {fmt.format(instr)}{mark}")

TARGETS = {
    "Faction::getName":              (0x286780, 0x60),
    "Faction::getData":              (0x6E000,  0x40),
    "RootObjectBase::getGameData":   (0xD1D30,  0x20),
    "RootObjectBase::getName":       (0xD3CA0,  0x80),
    "FactionMgr::getFactionByStringID": (0x2E7570, 0xE0),
    "FactionMgr::getFactionByName":  (0x2E74A0, 0xD0),
    "FactionMgr::getAllFactions":    (0x877240, 0x20),
    "Faction::isThePlayer":          (0xD5FC0,  0x30),
    "Faction::isNotARealFaction":    (0x166EA0, 0x40),
}

if __name__=="__main__":
    sel = sys.argv[1] if len(sys.argv)>1 else "all"
    for name,(rva,ln) in TARGETS.items():
        if sel=="all" or sel.lower() in name.lower():
            disasm(rva, ln, name)
