# -*- coding: utf-8 -*-
# Resuelve thunks de attackTarget, mapea vtbls 0x6AADD88 etc, lee RTTI TypeDescriptor.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl

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
print("SECTIONS:")
for s in SECTIONS:
    print(f"  {s[0]:8} rva=0x{s[1]:X} vsize=0x{s[2]:X} rawoff=0x{s[3]:X} rawsize=0x{s[4]:X} (rva_end=0x{s[1]+max(s[2],s[4]):X})")

def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size:
                return raw_off + d
    return None

def rd_qword(rva):
    off = rva_to_off(rva)
    if off is None: return None
    return struct.unpack_from("<Q", DATA, off)[0]
def rd_dword(rva):
    off = rva_to_off(rva)
    if off is None: return None
    return struct.unpack_from("<I", DATA, off)[0]

def resolve_thunk(rva):
    off = rva_to_off(rva)
    if off is None: return None
    b = DATA[off:off+16]
    dec = Decoder(64, b, ip=IMAGE_BASE+rva)
    try:
        instr = next(iter(dec))
    except Exception:
        return None
    if instr.mnemonic == Mnemonic.JMP and instr.flow_control == FlowControl.UNCONDITIONAL_BRANCH:
        try:
            return instr.near_branch_target - IMAGE_BASE
        except Exception:
            return None
    return None

print("\n############ Thunks de attackTarget ############")
for t in [0x1E6F, 0x204FA, 0x3E8DD, 0x1A5B9, 0x2F220, 0x4A188]:
    real = resolve_thunk(t)
    print(f"  thunk 0x{t:X} -> {'0x%X'%real if real else 'no-jmp'}")

# RTTI: vtbl-8 -> CompleteObjectLocator; COL+0xC (rel) -> TypeDescriptor (RVA); TD+0x10 = nombre
def read_rtti_name(vtbl_rva):
    col_ptr = rd_qword(vtbl_rva - 8)
    if not col_ptr: return None
    col_rva = col_ptr - IMAGE_BASE
    # COL layout: +0x0 signature, +0x4 offset, +0x8 cdOffset, +0xC pTypeDescriptor (RVA, image-rel)
    td_rva = rd_dword(col_rva + 0xC)
    if not td_rva: return None
    # TypeDescriptor: +0x0 vfptr, +0x8 spare, +0x10 name (.?AV...)
    name_off = rva_to_off(td_rva + 0x10)
    if name_off is None: return None
    end = DATA.index(b"\x00", name_off)
    return DATA[name_off:end].decode("ascii","ignore"), col_rva, td_rva

print("\n############ RTTI de vtbls candidatas ############")
for vt in [0x6AADD88, 0x6AAE448, 0x6AADC68, 0x16BE448, 0x16BDC68, 0x16F67B8, 0x16F8A68]:
    q0 = rd_qword(vt)
    info = read_rtti_name(vt)
    if info:
        print(f"  vtbl 0x{vt:X}: RTTI='{info[0]}' (COL 0x{info[1]:X}, TD 0x{info[2]:X}) | slot0=0x{q0:X}" if q0 else f"  vtbl 0x{vt:X}: RTTI='{info[0]}'")
    else:
        print(f"  vtbl 0x{vt:X}: sin RTTI legible | slot0={'0x%X'%q0 if q0 else 'n/d'}")

# Slots de cada vtbl Task con thunk resuelto
print("\n############ Slots vtbl (thunk-resuelto) ############")
for vt in [0x6AADD88, 0x6AAE448, 0x6AADC68, 0x16BE448, 0x16BDC68]:
    print(f"\n--- vtbl 0x{vt:X} ---")
    for s in range(0, 10):
        q = rd_qword(vt + s*8)
        if q is None:
            print(f"  +0x{s*8:02X}: <sin mapear>"); continue
        fn = q - IMAGE_BASE
        real = resolve_thunk(fn)
        print(f"  +0x{s*8:02X}: 0x{fn:X}" + (f" -> 0x{real:X}" if real else ""))
