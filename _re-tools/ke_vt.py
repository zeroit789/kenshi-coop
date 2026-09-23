# ES: Lee los slots indicados de una vtable y, si el qword apunta a un thunk jmp (E9), resuelve el
#     destino real. Depende de ke_dis.load()/ke_dis._image (no definidos en el ke_dis.py versionado).
#     Uso: python ke_vt.py <vtable_rva_hex> <slot_hex> [...]
# EN: Reads the given slots of a vtable and, if the qword points to a jmp thunk (E9), resolves the real
#     target. Depends on ke_dis.load()/ke_dis._image (not defined in the versioned ke_dis.py).
#     Usage: python ke_vt.py <vtable_rva_hex> <slot_hex> [...]

# resuelve slot de vtable: lee qword en (vtable_rva + slot), si apunta a thunk jmp resuelve destino real - READ ONLY
# EN: resolves a vtable slot: reads the qword at (vtable_rva + slot); if it points to a jmp thunk, resolves the real target - READ ONLY
import sys, struct, ke_dis
ke_dis.load(); img=ke_dis._image; IB=0x140000000
vt=int(sys.argv[1],16)
for s in sys.argv[2:]:
    slot=int(s,16)
    q=struct.unpack_from("<Q",img,vt+slot)[0]
    rva=q-IB
    real=rva; extra=""
    if 0<=rva<len(img) and img[rva]==0xE9:
        rel=struct.unpack_from("<i",img,rva+1)[0]; real=rva+5+rel; extra=f" -> jmp -> 0x{real:X}"
    print(f"vt+0x{slot:X}: qword=0x{q:X} (rva 0x{rva:X}){extra}")
