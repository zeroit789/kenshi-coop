# -*- coding: utf-8 -*-
# ES: Determina el layout exacto de lektor<Tasker*> (la cola en AITaskSytem+0x2E8): desensambla el bloque
#     del ctor que lo inicializa, dos usos ([rax+0x2E8] en 0x32E020 y la rutina que itera la cola en
#     0x34DB70) y busca accesos +0x2E8 en el slot 1 de AITaskSytem (0x5090B0). Uso: python probe_lektor.py
# EN: Determines the exact layout of lektor<Tasker*> (the queue at AITaskSytem+0x2E8): disassembles the ctor
#     block that initializes it, two uses ([rax+0x2E8] at 0x32E020 and the queue-iterating routine at
#     0x34DB70) and looks for +0x2E8 accesses in AITaskSytem slot 1 (0x5090B0). Usage: python probe_lektor.py

"""Determina el layout EXACTO de lektor<Tasker*> (la cola en AITaskSytem+0x2E8).
Desensambla codigo que carga [obj+0x2E8] y opera sobre el lektor (push/size)."""
from re_task_system import pe, disasm, print_disasm, BASE, follow_thunk
from iced_x86 import Mnemonic, OpKind

# 1) El ctor del lektor: en AITaskSytem::ctor (0x50DB70) el lektor @+0x2E8 se
#    inicializa por el bloque @0x50DBB6..0x50DC5E (vtable 0x16E34D0 NO, esa es
#    +0x2E8 distinto). Releer: en el ctor, +0x2E8 se construye en 0x50DC6B (lea rbx,[rsi+0x2E8]).
# EN: 1) The lektor ctor: in AITaskSytem::ctor (0x50DB70) the lektor at +0x2E8 is
#        initialized by block 0x50DBB6..0x50DC5E (vtable 0x16E34D0 NO, that is a
#        different +0x2E8). Re-read: in the ctor, +0x2E8 is built at 0x50DC6B (lea rbx,[rsi+0x2E8]).
print("===== Bloque del ctor AITaskSytem que inicializa +0x2E8 (lektor) =====")
print_disasm(0x50DC6B, 60, label="init lektor @+0x2E8")

# 2) Un acceso claro: 0x32E037 mov rcx,[rax+0x2E8] -> seguir que hace con el lektor
# EN: 2) A clear access: 0x32E037 mov rcx,[rax+0x2E8] -> follow what it does with the lektor
print("\n\n===== Uso del lektor: 0x32E020 (contexto de [rax+0x2E8]) =====")
print_disasm(0x32E020, 140, label="uso lektor (push/iter?)")

# 3) Otro: 0x34DB80 (mov rcx,[rbx+0x2E8]) zona con muchos accesos -> AItaskSytem real
# EN: 3) Another: 0x34DB80 (mov rcx,[rbx+0x2E8]) area with many accesses -> real AItaskSytem
print("\n\n===== 0x34DB70: rutina que itera la cola =====")
print_disasm(0x34DB70, 200, label="iter cola")

# 4) Slot1 de AITaskSytem (0x5090B0) parece 'addTask' (grande). Veamos si toca +0x2E8.
# EN: 4) AITaskSytem slot1 (0x5090B0) looks like 'addTask' (large). Let us see whether it touches +0x2E8.
print("\n\n===== AITaskSytem slot1 0x5090B0 (candidato addTask) - busca +0x2E8 =====")
for r,raw,s,ins in disasm(0x5090B0, 600):
    if "0x2e8" in s.lower() or "+0x2E8" in s:
        print(f"   0x{r:X}: {s}")
    if ins.mnemonic==Mnemonic.RET and r>0x5090B0+0x40:
        pass
