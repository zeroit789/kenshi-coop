import ke_re as k
import struct
IB=0x140000000
def u64(b): return struct.unpack('<Q',b[:8])[0]
def u32(b): return struct.unpack('<I',b[:4])[0]
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
pat = None
# buscar el patron de carga 'mov rax,[142133308h]' = 48 8B 05 <rel>. rel = 0x142133308 - (rip)
# es mas facil: ya tenemos el contexto. El singleton +0x60 -> vtable. 
# Identifiquemos la CLASE del 'this' (rbx) de 0x10DF78: la funcion 0x10D99B.
print("=== prologo func 0x10D99B (contiene 0x10DF78) ===")
print(k.disasm(0x10D99B, 0x80, 30))
