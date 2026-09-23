# ES: Dado uno o más RVAs, lista (1) las instrucciones de .text con operando RIP-relativo que apuntan a
#     ellos y (2) los qwords absolutos (base+RVA) en .rdata/.data (vtables, tablas de punteros).
#     Usa re_kenshi.py (pe, data, IMAGEBASE). Uso: python find_refs.py <rva_hex> [...]
# EN: Given one or more RVAs, lists (1) .text instructions with a RIP-relative operand pointing to
#     them and (2) absolute qwords (base+RVA) in .rdata/.data (vtables, pointer tables).
#     Uses re_kenshi.py (pe, data, IMAGEBASE). Usage: python find_refs.py <rva_hex> [...]

from re_kenshi import *
import struct
# Buscar referencias RIP-relativas (lea/mov) a un RVA destino en todo .text
# y referencias absolutas en .rdata (vtables)
# EN: Find RIP-relative references (lea/mov) to a target RVA across .text
#     and absolute references in .rdata (vtables)
def find_lea_refs(target_rvas, label=""):
    targets = set(target_rvas)
    # 1) referencias RIP-relativas en .text: lea reg,[rip+disp] o mov ... patrones 48 8D / 4C 8D / 48 8B etc
    # Mejor: decodificar linealmente todo .text y mirar memory_displacement
    # EN: 1) RIP-relative references in .text: lea reg,[rip+disp] or mov ... patterns 48 8D / 4C 8D / 48 8B etc
    #     Better: decode all of .text linearly and look at the memory displacement
    from iced_x86 import Decoder, Mnemonic, OpKind
    text_start = 0x1000; text_end = 0x1671000
    chunk = data[text_start:text_end]
    dec = Decoder(64, chunk, ip=IMAGEBASE+text_start)
    hits = []
    for instr in dec:
        if instr.is_ip_rel_memory_operand:
            t = instr.ip_rel_memory_address - IMAGEBASE
            if t in targets:
                hits.append((instr.ip-IMAGEBASE, t, instr.mnemonic))
    print(f"[{label}] referencias RIP-rel en .text: {len(hits)}")
    for src,t,mn in hits[:40]:
        print(f"  @0x{src:X} -> 0x{t:X}  ({mn})")
    return hits

def find_abs_qword(target_rvas, label=""):
    # Buscar el qword absoluto (IMAGEBASE+rva) en .rdata/.data (vtables, tablas de punteros)
    # EN: Find the absolute qword (IMAGEBASE+rva) in .rdata/.data (vtables, pointer tables)
    for s in pe.sections:
        nm = s.Name.rstrip(b'\x00').decode('latin1')
        if nm not in ('.rdata','.data'): continue
        va=s.VirtualAddress; sz=s.Misc_VirtualSize
        raw = data[va:va+sz]
        for t in target_rvas:
            needle = struct.pack('<Q', IMAGEBASE+t)
            idx=0
            while True:
                p = raw.find(needle, idx)
                if p<0: break
                print(f"  [{label}] qword 0x{IMAGEBASE+t:X} en {nm} @RVA 0x{va+p:X}")
                idx=p+1

# ES: Punto de entrada: RVAs en hexadecimal por argumentos.
# EN: Entry point: RVAs in hex as arguments.
if __name__=='__main__':
    import sys
    rvas=[int(a,16) for a in sys.argv[1:]]
    find_lea_refs(rvas,'lea/mov')
    print('--- vtable/qword abs ---')
    find_abs_qword(rvas,'abs')
