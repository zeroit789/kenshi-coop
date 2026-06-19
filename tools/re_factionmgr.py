# -*- coding: utf-8 -*-
# Localiza FactionManager: RTTI, vtable, ctor, layout (array de Faction*), getter por nombre/id.
import struct, sys
from iced_x86 import (Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl, Register)

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
TEXT = next(s for s in SECTIONS if s[0]==".text")
RDATA = next(s for s in SECTIONS if s[0]==".rdata")
DATAS = next(s for s in SECTIONS if s[0]==".data")

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

def find_bytes(b, start=0, end=None):
    res=[]; i=start
    end = end if end else len(DATA)
    while True:
        p = DATA.find(b, i, end)
        if p==-1: break
        res.append(p); i=p+1
    return res

def find_rtti_typedesc(class_name):
    # _TypeDescriptor: vftable ptr (8) + 0 (8) + ".?AV<name>@@\0"
    pat = (".?AV"+class_name+"@@").encode()
    res = find_bytes(pat)
    return [(p, off_to_rva(p)) for p in res]

def find_xref_lea_mov(target_rva, types=("lea","mov")):
    """RIP-relative refs (48/4C 8D/8B/89) en .text apuntando a target_rva."""
    tva = IMAGE_BASE + target_rva
    t0 = TEXT[3]; tsz = TEXT[4]; trva = TEXT[1]
    out=[]
    d = DATA
    for i in range(t0, t0+tsz-7):
        b0=d[i]; b1=d[i+1]
        if b0 in (0x48,0x4C) and b1 in (0x8D,0x8B,0x89):
            modrm=d[i+2]; mod=(modrm>>6)&3; rm=modrm&7
            if mod==0 and rm==5:
                disp=struct.unpack_from("<i",d,i+3)[0]
                instr_rva = off_to_rva(i)
                if instr_rva is None: continue
                tgt = instr_rva+7+disp
                if tgt==target_rva:
                    out.append((instr_rva, {0x8D:"lea",0x8B:"mov",0x89:"movstore"}[b1]))
    return out

def find_vtable_for_typedesc(td_rva):
    """Localiza COL que referencia el TD, y la vtable que apunta al COL."""
    # COL (CompleteObjectLocator) contiene en +0xC el RVA del TD (32-bit, image-relative)
    # Buscamos en .rdata un dword == td_rva (image-relative)
    rd0=RDATA[3]; rdsz=RDATA[4]; rdrva=RDATA[1]
    cols=[]
    d=DATA
    for i in range(rd0, rd0+rdsz-4, 4):
        v=struct.unpack_from("<I",d,i)[0]
        if v==td_rva:
            col_rva = off_to_rva(i)  # este i podria ser el campo pTypeDescriptor del COL
            cols.append((i, col_rva))
    return cols

def disasm(start_rva, length, label, stop_int3=True, max_ins=999):
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
        if instr.flow_control in (FlowControl.CONDITIONAL_BRANCH,FlowControl.UNCONDITIONAL_BRANCH,
                                  FlowControl.CALL):
            try:
                t=instr.near_branch_target-IMAGE_BASE
                if t: mark=f"   ; -> 0x{t:X} ({sec_of(t)})"
            except: pass
        if instr.mnemonic==Mnemonic.INT3 and stop_int3:
            print(f"0x{rva:08X}  {rh:<26} {fmt.format(instr)} <--PAD"); break
        print(f"0x{rva:08X}  {rh:<26} {fmt.format(instr)}{mark}")

if __name__=="__main__":
    what=sys.argv[1] if len(sys.argv)>1 else "rtti"
    if what=="rtti":
        for cn in ["FactionManager","Faction","FactionRelations","GameData","PlayerInterface"]:
            tds=find_rtti_typedesc(cn)
            print(f"\n{cn}: TypeDescriptor en {[(hex(off_to_rva(p)) if off_to_rva(p) else hex(p)) for p,_ in tds]}")
    elif what=="vtbl":
        cn=sys.argv[2]
        tds=find_rtti_typedesc(cn)
        for p,_ in tds:
            td_rva=off_to_rva(p)-8  # TD empieza 8 bytes antes del string (vftable ptr campo)
            print(f"\n{cn} TD_rva(string-8)=0x{td_rva:X}")
            cols=find_vtable_for_typedesc(td_rva)
            for ci,crva in cols[:10]:
                # El campo pTD esta en COL+0xC. COL_base = crva-0xC
                col_base = crva-0xC
                print(f"  COL field@0x{crva:X} -> COL base 0x{col_base:X}")
                # vtable apunta a (COL base) en [vtbl-8]
                colva = IMAGE_BASE+col_base
                refs = find_bytes(struct.pack("<Q",colva), RDATA[3], RDATA[3]+RDATA[4])
                for r in refs:
                    vt_rva = off_to_rva(r)+8
                    print(f"     vtable @ RVA 0x{vt_rva:X}  (slot0=0x{struct.unpack_from('<Q',DATA,r+8)[0]-IMAGE_BASE:X})")
    elif what=="disasm":
        rva=int(sys.argv[2],16); ln=int(sys.argv[3],16) if len(sys.argv)>3 else 0x100
        disasm(rva,ln,f"f_{sys.argv[2]}")
    elif what=="xref":
        rva=int(sys.argv[2],16)
        refs=find_xref_lea_mov(rva)
        print(f"xrefs RIP-rel a 0x{rva:X}: {len(refs)}")
        for r,t in refs[:40]: print(f"  0x{r:X} {t}")
