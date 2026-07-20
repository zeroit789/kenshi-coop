import ke_re as k
import struct
IB=0x140000000
buf=bytearray()
pos=0x1000; END=0x1673000
while pos<END:
    n=min(0x40000, END-pos); b=k.bytes_at_rva(pos,n)
    if not b: break
    buf+=b; pos+=n
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
        print(f"  --- lea@0x{h:X}:")
        print("   ", k.disasm(h, 24, 4).replace("\n","\n    "))
