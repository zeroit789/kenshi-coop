# ES: Para RVAs dentro de una función (0x5BD2B3, 0x5BD467), retrocede hasta 0x600 bytes y lista los
#     "lea r11/rax, [rip+disp32]" cuyo destino cae en .rdata (posibles vtables instaladas).
#     Uso: python findstart.py
# EN: For RVAs inside a function (0x5BD2B3, 0x5BD467), walks back up to 0x600 bytes and lists the
#     "lea r11/rax, [rip+disp32]" whose target lands in .rdata (possible installed vtables).
#     Usage: python findstart.py

import ke_re as k
import struct
IMG=0x140000000
def u64(b): return struct.unpack("<Q",b)[0]

# Buscar hacia atras el prologo de la funcion que contiene un RVA dado,
# y reportar el primer 'lea reg,[vtable]; mov [reg_this],reg' que instale vtable.
# EN: Walk back to the prologue of the function containing a given RVA,
#     and report the first lea reg,[vtable]; mov [reg_this],reg that installs a vtable
#     (in practice it reports every lea into .rdata in the window).
def scan_back_for_vtable(rva, maxback=0x400):
    data = bytes(k.bytes_at_rva(rva-maxback, maxback+0x10))
    base = rva-maxback
    hits=[]
    for i in range(len(data)-7):
        # lea r11/rax,[rip+disp32]: 4C 8D 1D dd dd dd dd  o  48 8D 05 dd dd dd dd
        # EN: lea r11/rax,[rip+disp32]: 4C 8D 1D dd dd dd dd  or  48 8D 05 dd dd dd dd
        if (data[i]==0x4C and data[i+1]==0x8D and data[i+2]==0x1D) or \
           (data[i]==0x48 and data[i+1]==0x8D and data[i+2]==0x05):
            disp=struct.unpack("<i",data[i+3:i+7])[0]
            instr_rva=base+i
            tgt=instr_rva+7+disp
            # solo vtables plausibles en .rdata
            # EN: only plausible vtables in .rdata
            if 0x1673000 <= tgt <= 0x1BBE000:
                hits.append((hex(instr_rva),hex(tgt)))
    return hits

for rva in [0x5bd2b3, 0x5bd467]:
    print("=== contexto vtables LEA antes de", hex(rva), "===")
    for h in scan_back_for_vtable(rva, 0x600):
        print("  ", h)
