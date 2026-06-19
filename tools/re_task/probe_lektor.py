# -*- coding: utf-8 -*-
"""Determina el layout EXACTO de lektor<Tasker*> (la cola en AITaskSytem+0x2E8).
Desensambla codigo que carga [obj+0x2E8] y opera sobre el lektor (push/size)."""
from re_task_system import pe, disasm, print_disasm, BASE, follow_thunk
from iced_x86 import Mnemonic, OpKind

# 1) El ctor del lektor: en AITaskSytem::ctor (0x50DB70) el lektor @+0x2E8 se
#    inicializa por el bloque @0x50DBB6..0x50DC5E (vtable 0x16E34D0 NO, esa es
#    +0x2E8 distinto). Releer: en el ctor, +0x2E8 se construye en 0x50DC6B (lea rbx,[rsi+0x2E8]).
print("===== Bloque del ctor AITaskSytem que inicializa +0x2E8 (lektor) =====")
print_disasm(0x50DC6B, 60, label="init lektor @+0x2E8")

# 2) Un acceso claro: 0x32E037 mov rcx,[rax+0x2E8] -> seguir que hace con el lektor
print("\n\n===== Uso del lektor: 0x32E020 (contexto de [rax+0x2E8]) =====")
print_disasm(0x32E020, 140, label="uso lektor (push/iter?)")

# 3) Otro: 0x34DB80 (mov rcx,[rbx+0x2E8]) zona con muchos accesos -> AItaskSytem real
print("\n\n===== 0x34DB70: rutina que itera la cola =====")
print_disasm(0x34DB70, 200, label="iter cola")

# 4) Slot1 de AITaskSytem (0x5090B0) parece 'addTask' (grande). Veamos si toca +0x2E8.
print("\n\n===== AITaskSytem slot1 0x5090B0 (candidato addTask) - busca +0x2E8 =====")
for r,raw,s,ins in disasm(0x5090B0, 600):
    if "0x2e8" in s.lower() or "+0x2E8" in s:
        print(f"   0x{r:X}: {s}")
    if ins.mnemonic==Mnemonic.RET and r>0x5090B0+0x40:
        pass
