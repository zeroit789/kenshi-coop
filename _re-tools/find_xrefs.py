from re_kenshi import *
import struct
from iced_x86 import Decoder, Mnemonic, OpKind

# Buscar TODOS los call/jmp a un RVA destino (directos y via thunk de 5 bytes E9)
def find_callers(target_rva, scan_start=0x1000, scan_end=0x1671000):
    target_abs = IMAGEBASE + target_rva
    results_direct = []
    # 1) Identificar thunks (jmp E9) que apuntan a target
    thunks = []
    # escaneo lineal de bytes buscando E9 con destino == target (thunks ILT)
    # los thunks estan en .text baja; escaneamos todo .text
    chunk = data[scan_start:scan_end]
    base = scan_start
    i = 0
    L = len(chunk)
    while i < L-5:
        if chunk[i] == 0xE9:
            rel = struct.unpack_from('<i', chunk, i+1)[0]
            dest = base + i + 5 + rel
            if dest == target_rva:
                thunks.append(base+i)
            i += 1
        else:
            i += 1
    return thunks

if __name__ == '__main__':
    import sys
    tgt = int(sys.argv[1], 16)
    thunks = find_callers(tgt)
    print(f"Thunks (jmp 0x{tgt:X}): {[hex(t) for t in thunks]}")
    # Ahora buscar calls directos a tgt Y a cada thunk
    targets = set([tgt] + thunks)
    # escaneo de calls E8
    chunk = data[0x1000:0x1671000]
    base = 0x1000
    L = len(chunk)
    callers = []
    i = 0
    while i < L-5:
        if chunk[i] == 0xE8:
            rel = struct.unpack_from('<i', chunk, i+1)[0]
            dest = base + i + 5 + rel
            if dest in targets:
                callers.append((base+i, dest))
        i += 1
    print(f"Total callers (E8) a target o sus thunks: {len(callers)}")
    for src, dst in callers[:60]:
        print(f"  call @0x{src:X} -> 0x{dst:X}")
