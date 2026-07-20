import ke_re as k
import struct

IB = 0x140000000
def u64(b): return struct.unpack('<Q', b[:8])[0]
def u32(b): return struct.unpack('<I', b[:4])[0]
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
print("===== ctor/factory 0x140AD98A0 (usado por B01F6F) =====")
print(k.disasm(0xAD98A0, 0x90, 40))
