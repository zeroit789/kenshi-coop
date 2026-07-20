# -*- coding: utf-8 -*-
# READ-ONLY: busca CALLs/JMPs directos (E8/E9 rel32) a un RVA objetivo en .text, incl. via thunk.
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
def get_text():
    for n,sr,vs,ro,rs in SECTIONS:
        if n==".text": return sr,ro,rs
TEXT_RVA, TEXT_OFF, TEXT_SIZE = get_text()
def off_to_rva(off): return TEXT_RVA + (off - TEXT_OFF)

def find_callers(target_rva, want_jmp=False):
    res=[]
    end = TEXT_OFF + TEXT_SIZE
    i = TEXT_OFF
    while i < end-5:
        b = DATA[i]
        if b in (0xE8, 0xE9):  # call rel32 / jmp rel32
            rel = struct.unpack_from("<i", DATA, i+1)[0]
            src_rva = off_to_rva(i)
            dst = src_rva + 5 + rel
            if dst == target_rva:
                kind = "JMP(thunk)" if b==0xE9 else "CALL"
                res.append((src_rva, kind))
        i += 1
    return res

if __name__ == "__main__":
    tgt = int(sys.argv[1],16)
    callers = find_callers(tgt)
    print(f"Refs directas a 0x{tgt:X}: {len(callers)}")
    thunks=[]
    for rva,kind in callers:
        print(f"  0x{rva:X}  {kind}")
        if kind.startswith("JMP"): thunks.append(rva)
    # callers de cada thunk (un nivel)
    for th in thunks:
        sub = find_callers(th)
        print(f"  -- callers del thunk 0x{th:X}: {len(sub)}")
        for rva,kind in sub[:30]:
            print(f"       0x{rva:X}  {kind}")
