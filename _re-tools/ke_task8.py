# ES: Para 4 escrituras candidatas a +0xE8, estima el inicio de la función contenedora (relleno int3)
#     y desensambla desde ahí hasta la escritura buscando referencias a las vtables AnimationClass,
#     Tasker, Task_MeleeAttack y al campo +0x448. Usa ke_re. Uso: python ke_task8.py
# EN: For 4 candidate +0xE8 writes, estimates the start of the containing function (int3 padding) and
#     disassembles from there up to the write looking for references to the AnimationClass, Tasker
#     and Task_MeleeAttack vtables and to field +0x448. Uses ke_re. Usage: python ke_task8.py

import ke_re as k
import struct
IB=0x140000000
def u64(b): return struct.unpack('<Q',b[:8])[0]
def u32(b): return struct.unpack('<I',b[:4])[0]
# ES: Nombre RTTI de una vtable o None (no se usa aquí).
# EN: RTTI name of a vtable or None (unused here).
def rtti_name(vt):
    try:
        col=u64(k.bytes_at_rva(vt-8,8))
        if col<IB or col>IB+0x2000000: return None
        c=k.bytes_at_rva(col-IB,0x10); td=u32(c[0xC:0x10])
        return k.read_string_near(td+0x10,80)
    except: return None

# 0x10DF78: singleton [0x142133308] -> obj +0x60 -> vtable -> call vt[0x1D0].
# El singleton en 0x142133308 (data). No podemos resolver runtime. Pero veamos
# si esta funcion contenedora instala vtable AnimationClass. Encontrar prologo.
# EN: 0x10DF78: singleton [0x142133308] -> obj +0x60 -> vtable -> call vt[0x1D0].
#     The singleton at 0x142133308 (data). We cannot resolve it at runtime. But let us see
#     whether this containing function installs the AnimationClass vtable. Find the prologue.
# Buscar hacia atras el inicio de funcion (int3 padding + push/sub rsp).
# EN: Walk back to the function start (int3 padding + push/sub rsp).
def find_func_start(rva, maxback=0x2000):
    raw = k.bytes_at_rva(rva-maxback, maxback)
    # buscar ultima secuencia CC CC seguida de un prologo plausible antes de rva
    # EN: find the last CC CC sequence followed by a plausible prologue before rva
    #     (in practice: the last CC followed by a non-CC byte)
    best=None
    for i in range(len(raw)-1):
        if raw[i]==0xCC and raw[i+1]!=0xCC:
            cand = rva-maxback+i+1
            best=cand
    return best

for rva in [0x10DF78, 0x72A9A3, 0x72B210, 0x7DC0AA]:
    fs=find_func_start(rva)
    if fs:
        blk=k.disasm(fs, min(rva-fs+12, 0x1500), 700)
        hasAnim=('16f4588' in blk.lower()) or ('16f10e8' in blk.lower())
        has448='448h' in blk
        # contar refs a vtables conocidas Task
        # EN: count refs to known Task vtables
        hasTasker=('16bdc68' in blk.lower())
        hasMelee=('16be448' in blk.lower())
        print(f"0x{rva:X} funcstart~0x{fs:X} len=0x{rva-fs:X} | anim={hasAnim} c448={has448} tasker={hasTasker} melee={hasMelee}")
    else:
        print(f"0x{rva:X} no funcstart")
