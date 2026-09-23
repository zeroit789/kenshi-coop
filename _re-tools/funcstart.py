# ES: Estima el inicio de la función que contiene 0x5BD2B3 buscando el último relleno int3 (CC CC)
#     antes del RVA y desensambla 20 instrucciones desde ahí. Uso: python funcstart.py
# EN: Estimates the start of the function containing 0x5BD2B3 by finding the last int3 padding (CC CC)
#     before the RVA and disassembles 20 instructions from there. Usage: python funcstart.py

import ke_re as k
import struct
# Buscar el inicio de funcion: secuencia de int3 (CC CC) seguida de prologo, antes del RVA dado
# EN: Find the function start: a run of int3 (CC CC) followed by a prologue, before the given RVA
def find_start(rva, maxback=0x1200):
    data = bytes(k.bytes_at_rva(rva-maxback, maxback))
    base = rva-maxback
    # buscar ultima ocurrencia de CC CC ... que preceda a un prologo
    # EN: find the last CC CC ... occurrence preceding a prologue
    last=None
    for i in range(len(data)-4):
        if data[i]==0xCC and data[i+1]==0xCC and data[i+2]!=0xCC:
            # candidato a inicio de funcion en i+2 (saltando padding)
            # EN: candidate function start at i+2 (skipping padding)
            j=i+1
            while j<len(data) and data[j]==0xCC: j+=1
            last=base+j
    return last

for rva in [0x5bd2b3]:
    s=find_start(rva)
    print("inicio funcion que contiene", hex(rva), "=", hex(s))
    print(k.disasm(s, 0x40, 20))
