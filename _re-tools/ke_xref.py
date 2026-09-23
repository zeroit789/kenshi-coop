# ES: Lista (hasta 121) instrucciones de .text con operando de memoria [reg+disp] para el
#     desplazamiento dado, filtrando escrituras, lecturas o ambas. Decodificación lineal (puede
#     desincronizarse). Uso: python ke_xref.py <disp_hex> [write|read|both]
# EN: Lists (up to 121) .text instructions with a [reg+disp] memory operand for the given displacement,
#     filtering writes, reads or both. Linear decoding (may desync).
#     Usage: python ke_xref.py <disp_hex> [write|read|both]

# Busca instrucciones que escriben/leen [reg+disp] con un displacement dado en .text. READ-ONLY.
# Uso: python ke_xref.py <disp_hex> [write|read|both]
# EN: Finds instructions that write/read [reg+disp] with a given displacement in .text. READ-ONLY.
#     Usage: python ke_xref.py <disp_hex> [write|read|both]
import sys, pefile, struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, OpKind, Mnemonic, Register
EXE = r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
IB = 0x140000000
pe = pefile.PE(EXE, fast_load=True)
data = open(EXE,"rb").read()
text=None
for s in pe.sections:
    if b".text" in s.Name: text=s;break
start=text.PointerToRawData; end=start+text.SizeOfRawData; base=text.VirtualAddress
disp=int(sys.argv[1],16)
mode=sys.argv[2] if len(sys.argv)>2 else "both"
# ES: decodificar linealmente desde el inicio de .text; aproximado (se desincroniza pero vale para escanear)
#     Alternativa descartada: decodificar en cada offset es muy pesado. Estrategia: una pasada lineal de .text.
# decode linear from .text start; approximate (will desync but good enough for scan windows)
# Instead, scan for the encoded disp32/disp8 in mov with mem operand by brute decoding every offset is heavy.
# Strategy: decode the whole .text once linearly.
b=data[start:end]
dec=Decoder(64,b,ip=IB+base)
fmt=Formatter(FormatterSyntax.INTEL)
hits=[]
for ins in dec:
    if ins.memory_displacement == disp and ins.memory_base != Register.NONE and ins.memory_base != Register.RIP:
        m=fmt.format(ins)
        # ES: heurística escritura vs lectura: el destino es memoria si el primer operando es memoria
        # heuristic write vs read: dest is memory if first op is mem
        is_w = ins.op0_kind==OpKind.MEMORY
        if mode=="write" and not is_w: continue
        if mode=="read" and is_w: continue
        hits.append((ins.ip-IB, m, is_w))
        if len(hits)>120: break
for rva,m,w in hits:
    print(f"0x{rva:X}: [{'W' if w else 'R'}] {m}")
print(f"--- {len(hits)} hits for disp 0x{disp:X} ({mode}) ---")
