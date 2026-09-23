# ES: Serie "ke_*" (solo lectura del binario). Lista las instrucciones call rel32 (E8) de .text cuyo
#     destino es el RVA pedido, directamente o a través de un thunk jmp E9 de un nivel. Lee el .exe
#     por offsets de fichero. Uso: python ke_callers.py <rva_hex>
# EN: "ke_*" series (read-only on the binary). Lists .text rel32 call instructions (E8) whose target
#     is the requested RVA, directly or through a one-level E9 jmp thunk. Reads the .exe by file
#     offsets. Usage: python ke_callers.py <rva_hex>

# Busca callers: instrucciones call rel32 (E8) cuyo destino (resolviendo 1 thunk jmp) == target. READ-ONLY.
# EN: Finds callers: call rel32 (E8) instructions whose target (resolving 1 jmp thunk) == target. READ-ONLY.
import sys,struct,pefile
EXE=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(EXE,fast_load=True); data=open(EXE,"rb").read()
for s in pe.sections:
    if b".text" in s.Name: text=s;break
base=text.VirtualAddress; raw=text.PointerToRawData; size=text.SizeOfRawData
img=data
# ES: Convierte un RVA de .text en offset del fichero.
# EN: Converts a .text RVA into a file offset.
def rva2off(r): return raw+(r-base)
target=int(sys.argv[1],16)
# tambien aceptar destino via thunk: precomputar set de RVAs que saltan a target
# EN: also accept a target through a thunk (done inline below: 1 thunk level is resolved per call)
hits=[]
for r in range(base, base+size-5):
    o=rva2off(r)
    if img[o]==0xE8:
        rel=struct.unpack_from("<i",img,o+1)[0]
        tgt=r+5+rel
        real=tgt
        # resolver 1 nivel de thunk
        # EN: resolve 1 thunk level
        to=rva2off(tgt) if base<=tgt<base+size else None
        if to is not None and img[to]==0xE9:
            rel2=struct.unpack_from("<i",img,to+1)[0]
            real=tgt+5+rel2
        if tgt==target or real==target:
            hits.append(r)
for h in hits: print(f"0x{h:X}")
print(f"--- {len(hits)} callers de 0x{target:X} ---")
