# ES: Investigación del campo +0xE8 de la animación: (1) comprueba si la función 0x5BD240 aparece en
#     las vtables AppearanceHuman/AppearanceAnimal (resolviendo thunks) y (2) lista las escrituras
#     "mov [reg+0xE8], reg" en la zona 0x5C0000-0x5F0000 para localizar el setter.
#     Uso: python anim_e8_setter.py
# EN: Investigation of animation field +0xE8: (1) checks whether function 0x5BD240 appears in the
#     AppearanceHuman/AppearanceAnimal vtables (resolving thunks) and (2) lists the
#     "mov [reg+0xE8], reg" writes in the 0x5C0000-0x5F0000 area to locate the setter.
#     Usage: python anim_e8_setter.py

import ke_re as k
import struct
IMG=0x140000000
def u64(b): return struct.unpack("<Q",b)[0]

# 1) Clase de 0x5bd240: ¿es metodo de AnimationClass? Buscar si 0x5bd240 (o thunk a el) esta en
#    vtable de AnimationClass / AnimationClassAnimal / Appearance.
# EN: 1) Class of 0x5bd240: is it an AnimationClass method? Check whether 0x5bd240 (or a thunk to it)
#    is in the AnimationClass / AnimationClassAnimal / Appearance vtable.
# ES: Devuelve los RVAs de los n primeros slots de la vtable, siguiendo los thunks jmp.
# EN: Returns the RVAs of the first n vtable slots, following jmp thunks.
def vt_slots(vt, n=60):
    out=[]
    for i in range(n):
        va=u64(bytes(k.bytes_at_rva(vt+i*8,8)))
        if va==0: out.append(0); continue
        rva=va-IMG
        # resolver thunk jmp
        # EN: resolve jmp thunk
        b=bytes(k.bytes_at_rva(rva,5))
        if b[0]==0xE9:
            disp=struct.unpack("<i",b[1:5])[0]
            rva=rva+5+disp
        out.append(rva)
    return out

for name,vt in [("AppearanceHuman",0x16E6338),("AppearanceAnimal",0x16E6598)]:
    sl=vt_slots(vt,30)
    print(name, "contiene 0x5bd240?", hex(0x5bd240) if 0x5bd240 in sl else "NO")
    print("  slots:", [hex(s) for s in sl[:12]])

# 2) Cluster: buscar escrituras mov [reg+0xE8],reg DENTRO de 0x5C0000..0x5F0000
#    y ademas lecturas mov reg,[reg+0xE8] para ver el setter.
# EN: 2) Cluster: find mov [reg+0xE8],reg writes INSIDE 0x5C0000..0x5F0000
#    and also mov reg,[reg+0xE8] reads to see the setter (only writes are implemented).
TEXT_START=0x1000; TEXT_END=0x1000+0x1671412
data=bytes(k.bytes_at_rva(0x5C0000, 0x30000))
base=0x5C0000
REX={0x48,0x4C,0x49,0x4D}
print("\n--- mov [reg+0xE8],reg en 0x5C0000-0x5F0000 ---")
for i in range(len(data)-7):
    if data[i] in REX and data[i+1]==0x89:
        m=data[i+2]; mod=(m>>6)&3; rm=m&7
        if mod==2 and rm!=4 and rm!=5 and data[i+3]==0xE8 and data[i+4]==0 and data[i+5]==0 and data[i+6]==0:
            print("  W", hex(base+i))
