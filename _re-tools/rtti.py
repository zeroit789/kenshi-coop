import ke_re as k
import struct

def u32(b): return struct.unpack("<I", b)[0]
def u64(b): return struct.unpack("<Q", b)[0]

IMG = 0x140000000

def rtti_name(vtable_rva):
    # COL en vtable-8
    col_off = vtable_rva - 8
    col_va = u64(bytes(k.bytes_at_rva(col_off,8)))
    if col_va == 0:
        return "(COL=0)"
    col_rva = col_va - IMG
    # COL layout: +0 sig, +4 offset, +8 cdOffset, +0xC pTypeDescriptor (RVA, 32-bit relative en x64)
    col = bytes(k.bytes_at_rva(col_rva,0x20))
    td_rva = u32(col[0xC:0x10])   # en x64 es un RVA de 32 bits relativo a ImageBase
    # TypeDescriptor: +0 vfptr, +8 spare, +0x10 name (mangled)
    name = k.read_string_near(td_rva+0x10, 64)
    return name

for vt in [0x16E6338, 0x16E6598, 0x16BDC68, 0x16BE448, 0x16F10E8, 0x16F4588]:
    try:
        print(hex(vt), "->", rtti_name(vt))
    except Exception as e:
        print(hex(vt), "ERR", e)
