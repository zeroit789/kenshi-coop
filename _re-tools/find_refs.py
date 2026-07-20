from re_kenshi import *
import struct
# Buscar referencias RIP-relativas (lea/mov) a un RVA destino en todo .text
# y referencias absolutas en .rdata (vtables)
def find_lea_refs(target_rvas, label=""):
    targets = set(target_rvas)
    # 1) referencias RIP-relativas en .text: lea reg,[rip+disp] o mov ... patrones 48 8D / 4C 8D / 48 8B etc
    # Mejor: decodificar linealmente todo .text y mirar memory_displacement
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

if __name__=='__main__':
    import sys
    rvas=[int(a,16) for a in sys.argv[1:]]
    find_lea_refs(rvas,'lea/mov')
    print('--- vtable/qword abs ---')
    find_abs_qword(rvas,'abs')
