# -*- coding: utf-8 -*-
"""Confirma que char+0x20 (AITaskSytem*) arranca NULL y se llena en AI::create.
Busca: (1) quien llama AI::create 0x622110 (xref), (2) lecturas/escrituras de char+0x20,
(3) el push real del lektor (addTask)."""
import struct
from re_task_system import pe, disasm, print_disasm, BASE, follow_thunk
from iced_x86 import Mnemonic, OpKind

text=pe.sec(".text"); tstart=text["ro"]; tsize=text["rs"]; trva=text["rva"]
blob=pe.data[tstart:tstart+tsize]

# 1) xrefs (call rel32) a AI::create 0x622110
print("===== callers de AI::create 0x622110 =====")
tgt=0x622110
cnt=0
for i in range(len(blob)-5):
    if blob[i]==0xE8:
        disp=struct.unpack_from("<i",blob,i+1)[0]
        src=trva+i
        if src+5+disp==tgt:
            print(f"  call AI::create desde RVA 0x{src:X}")
            cnt+=1
            if cnt>=12: break
print(f"  total mostrados: {cnt}")

# 2) accesos a char+0x20: distinguir 'mov [reg+0x20],rax' (set) vs 'mov rax,[reg+0x20]' (get)
#    Limitamos a contexto cercano a callers de spawn; aqui solo contamos.
print("\n===== escrituras 'mov [reg+0x20], rax/0' en .text (set de AICore) =====")
# patron: 48 89 4X 20  (mov [reg+0x20], reg) con modrm mod=01 disp8=0x20
sets=0; nulls=0
for i in range(len(blob)-4):
    if blob[i]==0x48 and blob[i+1]==0x89 and (blob[i+2]&0xC0)==0x40 and (blob[i+2]&7)!=4 and blob[i+3]==0x20:
        sets+=1
print(f"  count mov [reg+0x20],reg disp8=0x20: {sets} (incluye muchos structs)")

# 3) push real del lektor: funcion que toma (lektor*, Tasker**) y crece.
#    El AItaskSytem usa el lektor via metodo no-virtual. Buscamos call a una
#    funcion que lea [rcx+8](size),[rcx+0xC](cap),[rcx+0x10](data) en ese orden.
#    Escaneamos funciones pequenas con ese patron de bytes.
print("\n===== buscando push_back del lektor (size+8/cap+0xC/data+0x10) =====")
# patron de bytes tipico: 8B 41 08 (mov eax,[rcx+8]) ... 3B 41 0C (cmp eax,[rcx+0xC])
needle = bytes([0x8B,0x41,0x08])  # mov eax,[rcx+8]
idx=0; found=0
while found<8:
    p=blob.find(needle, idx)
    if p==-1: break
    idx=p+1
    # mirar ventana 16 bytes por cmp con [rcx+0xC]
    win=blob[p:p+24]
    if bytes([0x3B,0x41,0x0C]) in win or bytes([0x39,0x41,0x0C]) in win or bytes([0x41,0x0C]) in win:
        rva=trva+p
        print(f"\n  candidato push @0x{rva:X}:")
        for r,raw,s,ins in disasm(rva,40):
            print(f"     0x{r:X}: {s}")
            if ins.mnemonic==Mnemonic.RET: break
        found+=1
