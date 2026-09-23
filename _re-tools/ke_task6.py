# ES: Desensambla el factory/constructor 0xAD98A0, usado por la escritura candidata 0xB01F6F
#     (aloca y guarda en [rbx+0xE8]), para ver qué vtable instala. Usa ke_re. Uso: python ke_task6.py
# EN: Disassembles factory/constructor 0xAD98A0, used by candidate write 0xB01F6F (allocates and
#     stores into [rbx+0xE8]), to see which vtable it installs. Uses ke_re. Usage: python ke_task6.py

import ke_re as k
import struct

IB = 0x140000000
def u64(b): return struct.unpack('<Q', b[:8])[0]
def u32(b): return struct.unpack('<I', b[:4])[0]
# ES: Nombre RTTI de una vtable o None (no se usa aquí).
# EN: RTTI name of a vtable or None (unused here).
def rtti_name(vt_rva):
    try:
        col_va = u64(k.bytes_at_rva(vt_rva-8, 8))
        if col_va < IB or col_va > IB+0x2000000: return None
        col = k.bytes_at_rva(col_va-IB, 0x10)
        td_rva = u32(col[0xC:0x10])
        return k.read_string_near(td_rva+0x10, 80)
    except: return None

# ===========================================================
# CANDIDATA 0xB01F6F: aloca con 0x140AD98A0 y guarda en [rbx+0xE8].
# Ese 0x140AD98A0 es el "factory/ctor". Veamos que vtable instala.
# EN: CANDIDATE 0xB01F6F: allocates with 0x140AD98A0 and stores into [rbx+0xE8].
#     That 0x140AD98A0 is the "factory/ctor". Let us see which vtable it installs.
print("===== ctor/factory 0x140AD98A0 (usado por B01F6F) =====")
print(k.disasm(0xAD98A0, 0x90, 40))
