# ES: Lee todo .text con ke_re y, para las vtables Tasker (0x16BDC68) y Task_MeleeAttack (0x16BE448),
#     lista los lea RIP-relativos que apuntan exactamente a ellas y desensambla cada lea con las
#     instrucciones siguientes (para ver dónde se guarda la vtable). Uso: python ke_taskB.py
# EN: Reads all of .text through ke_re and, for the Tasker (0x16BDC68) and Task_MeleeAttack (0x16BE448)
#     vtables, lists the RIP-relative leas pointing exactly to them and disassembles each lea with the
#     following instructions (to see where the vtable is stored). Usage: python ke_taskB.py

import ke_re as k
import struct
IB=0x140000000
# ES: Carga .text en trozos de 0x40000 bytes.
# EN: Load .text in 0x40000-byte chunks.
buf=bytearray()
pos=0x1000; END=0x1673000
while pos<END:
    n=min(0x40000, END-pos); b=k.bytes_at_rva(pos,n)
    if not b: break
    buf+=b; pos+=n
# ES: RVAs de los lea RIP-relativos cuyo destino es targetrva.
# EN: RVAs of the RIP-relative leas whose target is targetrva.
def find_lea_to(targetrva):
    res=[]
    i=0; L=len(buf)
    while i<L-7:
        if buf[i] in (0x48,0x4C) and buf[i+1]==0x8D:
            modrm=buf[i+2]
            if (modrm>>6)&3==0 and modrm&7==5:
                disp=struct.unpack('<i',buf[i+3:i+7])[0]
                insn=0x1000+i; nxt=insn+7
                if nxt+disp == targetrva:
                    res.append(insn)
                i+=7; continue
        i+=1
    return res

for name,vt in [("Tasker",0x16BDC68),("Task_MeleeAttack",0x16BE448)]:
    print(f"\n===== LEA exactos a vtable {name} 0x{vt:X} =====")
    hits=find_lea_to(vt)
    print(f"  {len(hits)} hits")
    for h in hits:
        # mostrar la instruccion lea + las 2 siguientes para ver donde se guarda la vtable
        # EN: show the lea instruction + the next 2 to see where the vtable is stored
        print(f"  --- lea@0x{h:X}:")
        print("   ", k.disasm(h, 24, 4).replace("\n","\n    "))
