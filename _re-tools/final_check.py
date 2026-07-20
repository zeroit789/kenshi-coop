import ke_re as k
import struct
IMG=0x140000000
def u64(b): return struct.unpack("<Q",b)[0]
def u32(b): return struct.unpack("<I",b)[0]
def rtti_name(vt):
    col_va=u64(bytes(k.bytes_at_rva(vt-8,8)))
    if not (IMG<=col_va<=IMG+0x2000000): return "(no COL)"
    col=bytes(k.bytes_at_rva(col_va-IMG,0x14))
    td=u32(col[0xC:0x10])
    return k.read_string_near(td+0x10,80)

# La escritura 0x646aa2 escribia [rbx+0xE8]=lea 0x688968. Ver que es 0x688968 (¿vtable? ¿RTTI?)
# y la vtable 0x16F8BC8 que instalaba esa funcion en +0x190.
print("RTTI vtable 0x16F8BC8 (instalada en 0x646A80, base+0x190):", rtti_name(0x16F8BC8))
# 0x688968: ¿es una vtable? probar RTTI
print("0x688968 como vtable RTTI:", rtti_name(0x688968))
# ¿que hay en 0x688968? hex
print("bytes en 0x688968:", k.hexbytes(0x688968,24))

# Comparar layout vtable Tasker: vt[0]=dtor, vt[2]=vt+0x10=runAction
def slot(vt,i):
    va=u64(bytes(k.bytes_at_rva(vt+i*8,8))); 
    if not (IMG<=va<=IMG+0x1671412): return None
    r=va-IMG
    b=bytes(k.bytes_at_rva(r,5))
    if b[0]==0xE9: r=r+5+struct.unpack("<i",b[1:5])[0]
    return r
print("\nTasker 0x16BDC68: vt[0]=",hex(slot(0x16BDC68,0) or 0),"vt[2](+0x10)=",hex(slot(0x16BDC68,2) or 0))
print("Task_MeleeAttack 0x16BE448: vt[0]=",hex(slot(0x16BE448,0) or 0),"vt[2](+0x10)=",hex(slot(0x16BE448,2) or 0))
