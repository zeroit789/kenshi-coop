# ES: Desensambla el prólogo de la función 0x10D99B (contiene la escritura candidata 0x10DF78) para
#     identificar la clase del "this" (rbx) que recibe en +0xE8 el objeto fabricado vía un singleton.
#     Usa ke_re. Uso: python ke_task9.py
# EN: Disassembles the prologue of function 0x10D99B (contains candidate write 0x10DF78) to identify
#     the class of the "this" (rbx) that receives at +0xE8 the object built through a singleton.
#     Uses ke_re. Usage: python ke_task9.py

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

# CRUCIAL: el tick 0x5CCD90 hace mov r11,[rdi+448h]; mov rcx,[r11+0E8h]; vt[0x10].
# Confirmemos que [r11+0xE8] efectivamente se llama, y veamos si en el MISMO binario
# hay un sitio donde se asigne. Busquemos lecturas 'mov rXX,[rYY+448h]' seguidas pronto
# de escritura a +0xE8 (setter dentro de char). 
# Pero mejor: el objeto en +0xE8 lo crea un FACTORY virtual. En 0x10DF78 el factory es
# call [rax+0x1D0]. Veamos a que apunta el singleton 0x142133308 y su vtable, para saber
# que subsistema es (animacion? sonido? AI?).
# El singleton es dato runtime; resolvamos via xrefs: quien escribe 0x142133308.
# EN: CRUCIAL: tick 0x5CCD90 does mov r11,[rdi+448h]; mov rcx,[r11+0E8h]; vt[0x10].
#     Let us confirm that [r11+0xE8] is actually called, and see whether the SAME binary
#     has a place where it is assigned. Look for 'mov rXX,[rYY+448h]' reads soon followed
#     by a write to +0xE8 (setter inside char).
#     Better: the object at +0xE8 is created by a virtual FACTORY. At 0x10DF78 the factory is
#     call [rax+0x1D0]. Let us see what singleton 0x142133308 and its vtable point to, to know
#     which subsystem it is (animation? sound? AI?).
#     The singleton is runtime data; resolve it via xrefs: who writes 0x142133308.
pat = None
# buscar el patron de carga 'mov rax,[142133308h]' = 48 8B 05 <rel>. rel = 0x142133308 - (rip)
# es mas facil: ya tenemos el contexto. El singleton +0x60 -> vtable. 
# Identifiquemos la CLASE del 'this' (rbx) de 0x10DF78: la funcion 0x10D99B.
# EN: find the load pattern 'mov rax,[142133308h]' = 48 8B 05 <rel>. rel = 0x142133308 - (rip)
#     easier: we already have the context. The singleton +0x60 -> vtable.
#     Identify the CLASS of the 'this' (rbx) of 0x10DF78: function 0x10D99B.
print("=== prologo func 0x10D99B (contiene 0x10DF78) ===")
print(k.disasm(0x10D99B, 0x80, 30))
