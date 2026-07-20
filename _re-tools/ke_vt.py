# resuelve slot de vtable: lee qword en (vtable_rva + slot), si apunta a thunk jmp resuelve destino real - READ ONLY
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
