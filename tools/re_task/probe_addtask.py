# -*- coding: utf-8 -*-
# ES: Serie re_task (usa el helper re_task_system.py). Busca addTask: vuelca la vtable del "lektor"
#     (cola de Tasker* en AITaskSytem+0x2E8, vtable 0x16E34D0), desensambla sus slots 5 (+0x28) y 2
#     (+0x10) y el helper 0x4FDF4 que se llama tras cargar el lektor. Uso: python probe_addtask.py
# EN: re_task series (uses the re_task_system.py helper). Looks for addTask: dumps the vtable of the
#     "lektor" (Tasker* queue at AITaskSytem+0x2E8, vtable 0x16E34D0), disassembles its slots 5 (+0x28) and
#     2 (+0x10) and helper 0x4FDF4 called after loading the lektor. Usage: python probe_addtask.py

"""Localiza addTask: codigo que carga el lektor [obj+0x2E8] y escribe un Tasker*
incrementando size [lektor+8]. Examina los accesos en zona AITaskSytem 0x32Exxx/0x34Dxxx."""
import struct
from re_task_system import pe, disasm, print_disasm, BASE, follow_thunk
from iced_x86 import Mnemonic, OpKind

# El metodo del lektor que crece/inserta se llama via su vtable o como helper.
# En 0x34DB87 se hace: mov rcx,[rbx+0x2E8]; mov r10,[rcx]; call [r10+0x28] (slot 5 del lektor)
# y call [r10+0x10] (slot 2). Esos slots SI estan poblados aunque mi dump los corto.
# Volcamos la vtable del lektor leyendo punteros crudos (sin exigir .text-range estricto).
# EN: The lektor's grow/insert method is called through its vtable or as a helper.
#     At 0x34DB87: mov rcx,[rbx+0x2E8]; mov r10,[rcx]; call [r10+0x28] (lektor slot 5)
#     and call [r10+0x10] (slot 2). Those slots ARE populated even though my dump cut them.
#     We dump the lektor vtable reading raw pointers (without requiring a strict .text range).
print("===== vtable lektor 0x16E34D0: TODOS los qwords (crudos) =====")
for i in range(10):
    fp=pe.read_q(0x16E34D0+i*8)
    if fp is None: break
    rva=fp-BASE if fp>=BASE else fp
    real=follow_thunk(rva) if 0x1000<=rva<0x1673000 else rva
    d=disasm(real,6) if 0x1000<=real<0x1673000 else []
    first=d[0][2] if d else "(no .text)"
    print(f"  [{i}] +0x{i*8:X} -> 0x{rva:X} (real 0x{real:X})  {first}")

# slot [r10+0x28] = vtable index 5; [r10+0x10] = index 2. Desensamblamos esos.
# EN: slot [r10+0x28] = vtable index 5; [r10+0x10] = index 2. We disassemble those.
print("\n===== lektor vtable slot 5 (+0x28) - usado en iter/insert =====")
fp=pe.read_q(0x16E34D0+5*8)
if fp: print_disasm(follow_thunk(fp-BASE), 120, label="lektor slot5", stop_on_ret=True)

print("\n===== lektor vtable slot 2 (+0x10) =====")
fp=pe.read_q(0x16E34D0+2*8)
if fp: print_disasm(follow_thunk(fp-BASE), 120, label="lektor slot2", stop_on_ret=True)

# Y el add real desde AITaskSytem: 0x32E030 hace lea rdx,[rdi+0xE8]; mov rcx,[rax+0x2E8]; call 0x4FDF4
# 0x4FDF4 es helper sobre el lektor. Desensamblar.
# EN: And the real add from AITaskSytem: 0x32E030 does lea rdx,[rdi+0xE8]; mov rcx,[rax+0x2E8]; call 0x4FDF4
#     0x4FDF4 is a helper over the lektor. Disassemble it.
print("\n===== helper 0x4FDF4 (call tras cargar lektor) =====")
print_disasm(0x4FDF4, 160, label="helper lektor 0x4FDF4", stop_on_ret=True)
