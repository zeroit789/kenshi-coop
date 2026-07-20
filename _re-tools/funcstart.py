import ke_re as k
import struct
# Buscar el inicio de funcion: secuencia de int3 (CC CC) seguida de prologo, antes del RVA dado
def find_start(rva, maxback=0x1200):
    data = bytes(k.bytes_at_rva(rva-maxback, maxback))
    base = rva-maxback
    # buscar ultima ocurrencia de CC CC ... que preceda a un prologo
    last=None
    for i in range(len(data)-4):
        if data[i]==0xCC and data[i+1]==0xCC and data[i+2]!=0xCC:
            # candidato a inicio de funcion en i+2 (saltando padding)
            j=i+1
            while j<len(data) and data[j]==0xCC: j+=1
            last=base+j
    return last

for rva in [0x5bd2b3]:
    s=find_start(rva)
    print("inicio funcion que contiene", hex(rva), "=", hex(s))
    print(k.disasm(s, 0x40, 20))
