from re_kenshi import *
import struct
# Dado un RVA dentro de una vtable, encontrar el inicio de la vtable (retrocediendo hasta
# que el qword anterior NO sea un puntero a .text/.rdata o hasta el COL) y leer RTTI.
# COL (CompleteObjectLocator) esta en vtable[-1] (qword justo antes de vtable[0]).

def read_qword(rva):
    return struct.unpack_from('<Q', data, rva)[0]

def rtti_name_from_vtable_slot(slot_rva):
    # Retroceder para encontrar vtable[0]: el COL esta antes. Heuristica: buscar hacia atras
    # el primer qword que apunte a una direccion en .rdata que sea un COL valido.
    # Mas simple: probar varios offsets hacia atras leyendo COL en [vt-8].
    for back in range(0, 0x400, 8):
        vt0 = slot_rva - back
        col_ptr = read_qword(vt0 - 8)
        col_rva = col_ptr - IMAGEBASE
        if col_rva < 0 or col_rva > 0x1700000: continue
        # COL: +0x0 signature, +0xC offset... +0xC TypeDescriptor (rel), realmente
        # COL layout (x64): DWORD signature, DWORD offset, DWORD cdOffset, DWORD pTypeDescriptor(rva32),
        #                   DWORD pClassDescriptor(rva32), DWORD pSelf(rva32)
        try:
            sig, off_, cd, pTD = struct.unpack_from('<IIII', data, col_rva)
        except: continue
        if sig not in (0,1): continue
        td_rva = pTD  # ya es RVA de 32 bits (image-relative) en x64
        if td_rva <= 0 or td_rva > 0x1700000: continue
        # TypeDescriptor: +0x0 pVFTable(qword), +0x8 spare(qword), +0x10 name(char[])
        name_rva = td_rva + 0x10
        # leer string
        end = data.find(b'\x00', name_rva)
        nm = data[name_rva:end].decode('latin1', 'replace')
        if nm.startswith('.?A'):
            return vt0, nm
    return None, None

if __name__=='__main__':
    import sys
    for a in sys.argv[1:]:
        slot=int(a,16)
        vt0, nm = rtti_name_from_vtable_slot(slot)
        if nm:
            slotidx=(slot-vt0)//8
            print(f"slot @0x{slot:X}: vtable empieza 0x{vt0:X}, slot #{slotidx}, RTTI={nm}")
        else:
            print(f"slot @0x{slot:X}: no se pudo resolver RTTI")
