# ES: Para tres posiciones de .rdata donde aparece un puntero al thunk 0x477BC, retrocede hasta el
#     inicio de la vtable (primer qword que no apunta a .text) y calcula el índice/offset del slot.
#     Uso: python _slot.py. Imprime vtable, ocurrencia, índice y offset del slot.
# EN: For three .rdata positions holding a pointer to thunk 0x477BC, walks back to the start of the
#     vtable (first qword that does not point into .text) and computes the slot index/offset.
#     Usage: python _slot.py. Prints vtable, occurrence, slot index and slot offset.

import pefile
PATH=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(PATH, fast_load=True)
secs=[(s.Name.rstrip(b'\x00').decode('latin1'),s.VirtualAddress,s.Misc_VirtualSize) for s in pe.sections]
rd=[s for s in secs if s[0]=='.rdata'][0]
rdva,rdsz=rd[1],rd[2]; rdata=pe.get_data(rdva,rdsz)
thunk=IB+0x477bc
# Para cada ocurrencia, retroceder a inicio de vtable: vtable empieza tras un puntero RTTI (col) o tras 0s; aproximamos buscando hacia atras hasta que el qword previo NO sea un puntero a .text
# EN: For each occurrence, walk back to the vtable start: a vtable begins after an RTTI pointer (COL)
#     or after zeros; we approximate by walking back while the previous qword is a pointer into .text
# ES: ¿El qword (dirección absoluta) cae dentro del rango de .text? (límite superior aproximado)
# EN: Does the qword (absolute address) fall inside the .text range? (approximate upper bound)
def is_textptr(q):
    rva=q-IB
    return 0x1000<=rva<0x1673000
for occ in (0x16F2288,0x16F28C8,0x16F9F38):
    off=occ-rdva
    # subir hasta encontrar inicio (qword anterior no es text ptr -> es RTTI col ptr/typeinfo)
    # EN: walk up until the start (previous qword is not a text pointer -> it is the RTTI COL/typeinfo pointer)
    s=off
    while s-8>=0:
        q=int.from_bytes(rdata[s-8:s],'little')
        if is_textptr(q): s-=8
        else: break
    slot=(off-s)//8
    print(f"vtable@0x{rdva+s:X}  occ@0x{occ:X}  slot_index={slot}  slot_offset=0x{slot*8:X}")
