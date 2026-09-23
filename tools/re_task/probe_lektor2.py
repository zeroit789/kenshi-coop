# -*- coding: utf-8 -*-
# ES: Confirma el layout del lektor (+0x08 tamaño, +0x10 datos) y busca push/addTask: desensambla el malloc
#     del buffer en el ctor, vuelca los slots reales de la vtable 0x16E34D0 y marca los que tocan size y
#     data; muestra las primeras instrucciones de AITaskSytem::slot1. Uso: python probe_lektor2.py
# EN: Confirms the lektor layout (+0x08 size, +0x10 data) and looks for push/addTask: disassembles the
#     buffer malloc in the ctor, dumps the real slots of vtable 0x16E34D0 and flags those touching size and
#     data; shows the first instructions of AITaskSytem::slot1. Usage: python probe_lektor2.py

"""Confirma layout lektor (+0x08 size, +0x10 data) y localiza push/addTask."""
from re_task_system import pe, disasm, print_disasm, BASE, follow_thunk
from iced_x86 import Mnemonic, OpKind

# Ver ctor lektor completo (malloc del buffer)
# EN: See the full lektor ctor (buffer malloc)
print("===== ctor lektor (continuacion del malloc) 0x50DCA6 =====")
print_disasm(0x50DCA6, 40, label="malloc buffer")

# vtable lektor 0x16E34D0: volcar TODOS los slots reales siguiendo thunks
# EN: lektor vtable 0x16E34D0: dump ALL real slots following thunks
print("\n===== vtable lektor<Tasker*> 0x16E34D0 - slots reales =====")
for i in range(12):
    fp=pe.read_q(0x16E34D0+i*8)
    if not fp: break
    frva=fp-BASE
    if not (0x1000<=frva<0x1673000): break
    real=follow_thunk(frva)
    d=disasm(real,6)
    first=d[0][2] if d else ""
    print(f"  [{i}] slot+0x{i*8:X} -> 0x{frva:X} (real 0x{real:X})  {first}")

# Examinar slot con firma push_back: recibe (this, Tasker** o Tasker*), crece buffer.
# Heuristica: el slot que lee [this+8] (size) y [this+0xC] (cap), compara y escribe en [this+0x10].
# EN: Examine the slot with a push_back signature: takes (this, Tasker** or Tasker*), grows the buffer.
#     Heuristic: the slot reading [this+8] (size) and [this+0xC] (cap), comparing and writing to [this+0x10].
print("\n===== Inspeccion de cada slot del lektor buscando push_back =====")
for i in range(1,8):
    fp=pe.read_q(0x16E34D0+i*8)
    if not fp: break
    frva=fp-BASE
    if not (0x1000<=frva<0x1673000): break
    real=follow_thunk(frva)
    body=disasm(real,120)
    txt=" ".join(b[2] for b in body)
    # busca patron de size/cap (+8 / +0xC) y store a +0x10
    # EN: look for the size/cap pattern (+8 / +0xC) and a store to +0x10
    has8 = "+8]" in txt or "+0x8]" in txt
    hasC = "+0xc]" in txt.lower()
    has10 = "+0x10]" in txt.lower()
    tag=""
    if (has8 and has10): tag="  <<< CANDIDATO push/insert (toca size+data)"
    print(f"\n  --- slot {i} 0x{real:X}{tag} ---")
    for r,raw,s,ins in body[:26]:
        print(f"     0x{r:X}: {s}")
        if ins.mnemonic==Mnemonic.RET: break

# AITaskSytem slot1 (0x5090B0) - este es el grande; ver si encola tarea via +0x2E8
# EN: AITaskSytem slot1 (0x5090B0) - the big one; see whether it enqueues a task through +0x2E8
print("\n\n===== AITaskSytem slot1 0x5090B0 - primeras 60 instr (addTask?) =====")
print_disasm(0x5090B0, 320, label="AITaskSytem::slot1")
