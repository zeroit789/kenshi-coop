# ES: Para 7 escrituras candidatas a +0xE8, desensambla 0x250 bytes previos y comprueba si aparecen
#     referencias a +0x448 o a las vtables AnimationClass (0x16F4588 / 0x16F10E8).
#     Usa ke_re. Uso: python ke_task7.py
# EN: For 7 candidate +0xE8 writes, disassembles the previous 0x250 bytes and checks whether there
#     are references to +0x448 or to the AnimationClass vtables (0x16F4588 / 0x16F10E8).
#     Uses ke_re. Usage: python ke_task7.py

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

# Estrategia decisiva: en TODAS las funciones que contienen una candidata,
# buscar si dentro de la funcion se instala la vtable AnimationClass (lea ...,[0x16F4588] o [0x16F10E8])
# o se accede a char+0x448. Para eso necesito encontrar el inicio de cada funcion (prologo).
# Mas simple: escanear hacia atras desde la escritura hasta ~0x400 bytes buscando refs a esas vtables/0x448.

# EN: Decisive strategy: in ALL functions containing a candidate,
#     check whether the function installs the AnimationClass vtable (lea ...,[0x16F4588] or [0x16F10E8])
#     or accesses char+0x448. For that I need the start of each function (prologue).
#     Simpler: scan back from the write up to ~0x400 bytes looking for refs to those vtables/0x448.
VT_ANIM = 0x16F4588
VT_ANIMB= 0x16F10E8
cands = [0x10DF78,0x7DC0AA,0x7DC0CB,0x72B210,0x72B32A,0x72D4ED,0x72A9A3]

for rva in cands:
    found=[]
    # leer 0x300 bytes antes en crudo y buscar las constantes de vtable como desplazamientos rip-relativos es complejo;
    # en su lugar desensamblar 0x200 antes y buscar strings de las vtables resueltas y '448h'
    # EN: reading 0x300 raw bytes before and searching the vtable constants as rip-relative displacements is complex;
    #     instead disassemble 0x200 before (code: 0x250) and search for the resolved vtable strings and '448h'
    blk = k.disasm(rva-0x250, 0x250+12, 160)
    has448 = '448h' in blk
    hasAnim = ('16f4588' in blk.lower()) or ('16f10e8' in blk.lower())
    # buscar cualquier mov rXX,[rYY+448h]
    # EN: look for any mov rXX,[rYY+448h] (covered by the '448h' check)
    print(f"0x{rva:X}: 448h_ref={has448} animVtable_ref={hasAnim}")
