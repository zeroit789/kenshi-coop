# -*- coding: utf-8 -*-
# READ-ONLY: localiza vtable por nombre RTTI y vuelca N slots (resolviendo thunks JMP).
import struct, sys
from iced_x86 import Decoder, Mnemonic, FlowControl
EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
with open(EXE, "rb") as f: DATA = f.read()
e_lfanew = struct.unpack_from("<I", DATA, 0x3C)[0]; coff = e_lfanew + 4
num_sec = struct.unpack_from("<H", DATA, coff + 2)[0]; opt_size = struct.unpack_from("<H", DATA, coff + 16)[0]
sec_off = coff + 20 + opt_size; SECTIONS = []
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

def find_typedescriptor(name_substr):
    # busca el string .?AV<name>@@ en .data/.rdata
    needle = name_substr.encode("ascii")
    res = []
    start = 0
    while True:
        idx = DATA.find(needle, start)
        if idx < 0: break
        res.append(idx); start = idx+1
    return res

def off_to_rva(off):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if raw_off <= off < raw_off + raw_size:
            return srva + (off - raw_off)
    return None

def find_vtable_by_rtti(class_name):
    # class_name p.ej. "CombatClass"  -> busca ".?AVCombatClass@@"
    needle = (".?AV"+class_name+"@@").encode("ascii")
    idx = DATA.find(needle)
    if idx < 0: return None, None, None
    # TypeDescriptor empieza 0x10 antes del string
    td_off = idx - 0x10
    td_rva = off_to_rva(td_off)
    # buscar COL que referencia td_rva por RVA (campo +0xC), luego vtable cuyo [-8]=COL
    # escanear .rdata por dwords == td_rva
    cands = []
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if name not in (".rdata",".data"): continue
        for p in range(raw_off, raw_off+raw_size-4, 4):
            if struct.unpack_from("<I", DATA, p)[0] == td_rva:
                col_field_rva = off_to_rva(p)
                # +0xC del COL apunta a TD; COL base = col_field_rva - 0xC
                col_rva = col_field_rva - 0xC
                # buscar vtable: qword == (col_rva+IMAGE_BASE), vtable = ese+8
                col_va = col_rva + IMAGE_BASE
                for n2, sr2, vs2, ro2, rs2 in SECTIONS:
                    if n2 not in (".rdata",".data"): continue
                    for q in range(ro2, ro2+rs2-8, 8):
                        if struct.unpack_from("<Q", DATA, q)[0] == col_va:
                            vt_rva = off_to_rva(q) + 8
                            cands.append((td_rva, col_rva, vt_rva))
    return idx, td_rva, cands

if __name__ == "__main__":
    cls = sys.argv[1]; nslots = int(sys.argv[2]) if len(sys.argv)>2 else 80
    idx, td_rva, cands = find_vtable_by_rtti(cls)
    if idx is None:
        print(f"No se encontro RTTI .?AV{cls}@@"); sys.exit()
    print(f".?AV{cls}@@ string @file 0x{idx:X}  TD_rva=0x{td_rva:X}")
    seen=set()
    for td, col, vt in cands:
        if vt in seen: continue
        seen.add(vt)
        print(f"\nVTABLE 0x{vt:X} ({which_sec(vt)})  (COL 0x{col:X})")
        for s in range(0, nslots*8, 8):
            q = rd_qword(vt+s)
            if q is None: break
            slot_rva = q - IMAGE_BASE
            if which_sec(slot_rva) != ".text":
                break
            real = resolve_thunk(slot_rva)
            extra = f" =>0x{real:X}" if real else ""
            print(f"  +0x{s:03X}: 0x{slot_rva:X}{extra}")
