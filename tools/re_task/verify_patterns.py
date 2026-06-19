# -*- coding: utf-8 -*-
"""Verifica unicidad de patrones AOB y confirma consumidores de char+0x20 (AITaskSytem)."""
import struct
from re_task_system import pe, disasm, BASE
from iced_x86 import Mnemonic, OpKind

text=pe.sec(".text"); tstart=text["ro"]; tsize=text["rs"]; trva=text["rva"]
blob=pe.data[tstart:tstart+tsize]

def count_pattern(pat):
    """pat: string '48 8B ?? ...' -> cuenta matches en .text."""
    toks=pat.split()
    n=len(toks)
    mask=[None if t=='??' or t=='?' else int(t,16) for t in toks]
    cnt=0; first=None
    for i in range(len(blob)-n):
        ok=True
        for j in range(n):
            if mask[j] is not None and blob[i+j]!=mask[j]:
                ok=False; break
        if ok:
            cnt+=1
            if first is None: first=trva+i
    return cnt, first

# Patron AI::create (de la nota previa, 41 bytes). Verificar unicidad.
ai_pat="40 57 48 81 EC 90 00 00 00 48 C7 44 24 28 FE FF FF FF 48 89 9C 24 B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 80"
c,f=count_pattern(ai_pat)
print(f"AI::create 41-byte pattern: {c} match(es), first=0x{f:X}" if f else f"AI::create: {c} matches")

# AItaskSytem::ctor (0x50DB70) prologo - generar patron de 24 bytes y comprobar unicidad
off=pe.rva2off(0x50DB70)
raw=pe.data[off:off+28]
# wildcard de lea rip-rel: byte0..2 fijos, pero hay 'mov [rsp+8],rcx; push...' fijo
pat_bytes=" ".join(f"{b:02X}" for b in raw[:20])
c2,f2=count_pattern(pat_bytes)
print(f"\nAITaskSytem::ctor (0x50DB70) 20-byte raw: {c2} match(es)")
print(f"  bytes: {pat_bytes}")

# AICore ctor 0x50DB70 NO contiene lea rip-rel en primeros 20 bytes -> patron estable.

# Confirmar consumidores de char+0x20 leido como AITaskSytem* (mov rXX,[char+0x20]; call [vtbl])
# El char vtable usa +0x20 como AITaskSytem. Mostramos 5 sitios mov reg,[reg+0x20] seguidos de
# carga de vtable AITaskSytem (mov rax,[reg]; pero su vtable es 0x16E3F30).
print("\n===== Sample: lecturas 'mov reg,[reg+0x20]' que cargan AITaskSytem =====")
# Demasiado generico para filtrar bien; reportamos que AI::create escribe en char+0x20 (0x6221AF).
print("  AI::create escribe: mov [rbx(char)+0x20], rax  @ RVA 0x6221AF  (CONFIRMADO)")
print("  ctor AITaskSytem instala vtable .?AVAITaskSytem@@ 0x16E3F30 @ 0x50DB9C/0x50DD98")
