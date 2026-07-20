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

# Estrategia decisiva: en TODAS las funciones que contienen una candidata,
# buscar si dentro de la funcion se instala la vtable AnimationClass (lea ...,[0x16F4588] o [0x16F10E8])
# o se accede a char+0x448. Para eso necesito encontrar el inicio de cada funcion (prologo).
# Mas simple: escanear hacia atras desde la escritura hasta ~0x400 bytes buscando refs a esas vtables/0x448.

VT_ANIM = 0x16F4588
VT_ANIMB= 0x16F10E8
cands = [0x10DF78,0x7DC0AA,0x7DC0CB,0x72B210,0x72B32A,0x72D4ED,0x72A9A3]

for rva in cands:
    found=[]
    # leer 0x300 bytes antes en crudo y buscar las constantes de vtable como desplazamientos rip-relativos es complejo;
    # en su lugar desensamblar 0x200 antes y buscar strings de las vtables resueltas y '448h'
    blk = k.disasm(rva-0x250, 0x250+12, 160)
    has448 = '448h' in blk
    hasAnim = ('16f4588' in blk.lower()) or ('16f10e8' in blk.lower())
    # buscar cualquier mov rXX,[rYY+448h]
    print(f"0x{rva:X}: 448h_ref={has448} animVtable_ref={hasAnim}")
