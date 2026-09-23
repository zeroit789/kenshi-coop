# ES: Resuelve por RTTI las vtables que ke_task4 encontró como origen LEA de escrituras a +0xE8
#     (0x175D2B0, 0x1688968, 0x16C6C50, 0x16C6B68, 0x17085B0) y desensambla el contexto previo de
#     las escrituras cuyo origen es el retorno de un call. Usa ke_re. Uso: python ke_task5.py
# EN: Resolves via RTTI the vtables that ke_task4 found as LEA origin of +0xE8 writes (0x175D2B0,
#     0x1688968, 0x16C6C50, 0x16C6B68, 0x17085B0) and disassembles the preceding context of the writes
#     whose origin is a call return. Uses ke_re. Usage: python ke_task5.py

import ke_re as k
import struct

IB = 0x140000000
def u64(b): return struct.unpack('<Q', b[:8])[0]
def u32(b): return struct.unpack('<I', b[:4])[0]
# ES: Nombre RTTI de una vtable o None.
# EN: RTTI name of a vtable or None.
def rtti_name(vt_rva):
    try:
        col_va = u64(k.bytes_at_rva(vt_rva-8, 8))
        if col_va < IB or col_va > IB+0x2000000: return None
        col_rva = col_va - IB
        col = k.bytes_at_rva(col_rva, 0x10)
        td_rva = u32(col[0xC:0x10])
        return k.read_string_near(td_rva+0x10, 80)
    except: return None

# vtable instalada en 0x175D2B0 (de las LEA en B64BEA/B64EF4/B66514)
# EN: vtable installed at 0x175D2B0 (from the LEAs at B64BEA/B64EF4/B66514)
for vt in [0x175D2B0]:
    print(f"vtable @0x{vt:X} -> {rtti_name(vt)}")

# Resolver las LEA<-vtable de otras escrituras tambien:
# EN: Resolve the LEA<-vtable of other writes too:
for vt in [0x1688968, 0x16C6C50, 0x16C6B68, 0x17085B0]:
    print(f"vtable @0x{vt:X} -> {rtti_name(vt)}")

print("\n--- CALL_RET candidatas: ver que aloca/construye ---")
# 0x10DF78, 0x7DC0AA, 0x7DC0CB, 0xB01F6F : el call previo construye el objeto
# EN: 0x10DF78, 0x7DC0AA, 0x7DC0CB, 0xB01F6F : the previous call builds the object
for rva in [0x10DF78, 0x7DC0AA, 0x7DC0CB, 0xB01F6F]:
    print(f"\n##### Escritura 0x{rva:X} - contexto previo 0x60:")
    print(k.disasm(rva-0x60, 0x60+8, 30))
