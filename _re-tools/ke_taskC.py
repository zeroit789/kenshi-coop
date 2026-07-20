import ke_re as k
import struct
IB=0x140000000
buf=bytearray(); pos=0x1000; END=0x1673000
while pos<END:
    n=min(0x40000,END-pos); b=k.bytes_at_rva(pos,n)
    if not b: break
    buf+=b; pos+=n
def find_lea_to(t):
    res=[];i=0;L=len(buf)
    while i<L-7:
        if buf[i] in(0x48,0x4C) and buf[i+1]==0x8D:
            m=buf[i+2]
            if (m>>6)&3==0 and m&7==5:
                disp=struct.unpack('<i',buf[i+3:i+7])[0]
                insn=0x1000+i
                if insn+7+disp==t: res.append(insn)
                i+=7; continue
        i+=1
    return res

# De los 193 hits Tasker + Melee, ver si en las ~3 instrucciones siguientes hay un
# 'mov [reg+0E8h],' (que meteria la vtable o el objeto en +0xE8). Buscamos el byte E8 00 00 00
both=[]
for vt in (0x16BDC68,0x16BE448):
    for h in find_lea_to(vt):
        seg = bytes(buf[h-0x1000:h-0x1000+0x40])
        # buscar patron 89 .. E8 00 00 00 (mov [reg+0E8h],reg) cerca
        if b'\xe8\x00\x00\x00' in seg[:0x30]:
            both.append((h,vt))
print("hits Tasker/Melee con un +0xE8 cercano (<=0x30 bytes):", len(both))
for h,vt in both:
    print(f"  lea@0x{h:X} vt0x{vt:X}")
    print("   ", k.disasm(h,0x30,8).replace("\n","\n    "))

# Ahora: offset real del Tasker en Character. El tick lee char+0x448 (AnimationClassAnimal).
# El Tasker del personaje: busquemos lecturas 'mov rcx,[reg+OFF]; ...; call [Tasker_vt area]'.
# Mas directo: el objeto en animClass+0xE8 -> su vt[0x10] coincide con Tasker.runAction.
# Confirmemos cuantas vtables DISTINTAS tienen vt+0x10 == 0x7448F0 (Tasker.runAction) o similar,
# para saber si +0xE8 podria ser un Tasker real por identidad de slot.
print("\n=== vt[0] y vt[0x10] de Tasker 0x16BDC68 y de AnimationClass-embebido ===")
for vt in (0x16BDC68,0x16BE448):
    s0=struct.unpack('<Q',k.bytes_at_rva(vt,8))[0]
    s2=struct.unpack('<Q',k.bytes_at_rva(vt+0x10,8))[0]
    print(f"  vt0x{vt:X}: [0]=0x{s0-IB:X} [0x10]=0x{s2-IB:X}")
