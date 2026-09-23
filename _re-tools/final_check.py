# ES: Comprobaciones finales sobre vtables vía RTTI: nombre de clase de la vtable 0x16F8BC8, si
#     0x688968 es una vtable (y sus bytes), y los slots vt[0] (destructor) y vt[2] (+0x10, runAction)
#     de Tasker (0x16BDC68) y Task_MeleeAttack (0x16BE448). Uso: python final_check.py
# EN: Final checks on vtables through RTTI: class name of vtable 0x16F8BC8, whether 0x688968 is a
#     vtable (and its bytes), and slots vt[0] (destructor) and vt[2] (+0x10, runAction) of
#     Tasker (0x16BDC68) and Task_MeleeAttack (0x16BE448). Usage: python final_check.py

import ke_re as k
import struct
IMG=0x140000000
def u64(b): return struct.unpack("<Q",b)[0]
def u32(b): return struct.unpack("<I",b)[0]
# ES: Nombre RTTI de una vtable: vt-8 apunta al CompleteObjectLocator (COL); COL+0xC es el RVA del
#     TypeDescriptor y el nombre decorado empieza en TypeDescriptor+0x10.
# EN: RTTI name of a vtable: vt-8 points to the CompleteObjectLocator (COL); COL+0xC is the RVA of
#     the TypeDescriptor and the mangled name starts at TypeDescriptor+0x10.
def rtti_name(vt):
    col_va=u64(bytes(k.bytes_at_rva(vt-8,8)))
    if not (IMG<=col_va<=IMG+0x2000000): return "(no COL)"
    col=bytes(k.bytes_at_rva(col_va-IMG,0x14))
    td=u32(col[0xC:0x10])
    return k.read_string_near(td+0x10,80)

# La escritura 0x646aa2 escribia [rbx+0xE8]=lea 0x688968. Ver que es 0x688968 (¿vtable? ¿RTTI?)
# y la vtable 0x16F8BC8 que instalaba esa funcion en +0x190.
# EN: The write at 0x646aa2 wrote [rbx+0xE8]=lea 0x688968. Check what 0x688968 is (vtable? RTTI?)
#     and the vtable 0x16F8BC8 that installed that function at +0x190.
print("RTTI vtable 0x16F8BC8 (instalada en 0x646A80, base+0x190):", rtti_name(0x16F8BC8))
# 0x688968: ¿es una vtable? probar RTTI
# EN: 0x688968: is it a vtable? try RTTI
print("0x688968 como vtable RTTI:", rtti_name(0x688968))
# ¿que hay en 0x688968? hex
# EN: what is at 0x688968? hex
print("bytes en 0x688968:", k.hexbytes(0x688968,24))

# Comparar layout vtable Tasker: vt[0]=dtor, vt[2]=vt+0x10=runAction
# EN: Compare Tasker vtable layout: vt[0]=dtor, vt[2]=vt+0x10=runAction
# ES: RVA de la función en el slot i de la vtable (None si no es código), siguiendo thunks E9.
# EN: RVA of the function in vtable slot i (None if not code), following E9 thunks.
def slot(vt,i):
    va=u64(bytes(k.bytes_at_rva(vt+i*8,8))); 
    if not (IMG<=va<=IMG+0x1671412): return None
    r=va-IMG
    b=bytes(k.bytes_at_rva(r,5))
    if b[0]==0xE9: r=r+5+struct.unpack("<i",b[1:5])[0]
    return r
print("\nTasker 0x16BDC68: vt[0]=",hex(slot(0x16BDC68,0) or 0),"vt[2](+0x10)=",hex(slot(0x16BDC68,2) or 0))
print("Task_MeleeAttack 0x16BE448: vt[0]=",hex(slot(0x16BE448,0) or 0),"vt[2](+0x10)=",hex(slot(0x16BE448,2) or 0))
