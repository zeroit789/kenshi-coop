# ES: Imprime el slot vt+0x10 (índice 2, runAction en la jerarquía Tasker) de varias vtables
#     (Appearance*, Tasker, Task_MeleeAttack, AnimationClass*). Usa ke_re. Uso: python vtslots.py
# EN: Prints slot vt+0x10 (index 2, runAction in the Tasker hierarchy) of several vtables (Appearance*,
#     Tasker, Task_MeleeAttack, AnimationClass*). Uses ke_re. Usage: python vtslots.py

import ke_re as k
import struct
IMG=0x140000000
def u64(b): return struct.unpack("<Q",b)[0]
# ES: RVA de la función en el slot idx de la vtable (0 si el puntero es nulo).
# EN: RVA of the function in slot idx of the vtable (0 if the pointer is null).
def slot(vt, idx):
    off = vt + idx*8
    va = u64(bytes(k.bytes_at_rva(off,8)))
    return va-IMG if va else 0

# vt+0x10 = slot 2
# EN: vt+0x10 = slot 2
for name,vt in [("AppearanceHuman",0x16E6338),("AppearanceAnimal",0x16E6598),
                ("Tasker",0x16BDC68),("Task_MeleeAttack",0x16BE448),
                ("AnimationClass",0x16F10E8),("AnimationClassAnimal",0x16F4588)]:
    s2 = slot(vt,2)  # vt+0x10
    # resolver thunk si jmp
    # EN: resolve the thunk if it is a jmp (only 1 byte is read; not implemented)
    d = bytes(k.bytes_at_rva(s2,1))
    print(f"{name:22s} vt+0x10 -> {hex(s2)}")
