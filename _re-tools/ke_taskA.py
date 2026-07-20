import ke_re as k
import struct
IB=0x140000000
# Buscar refs RIP-relativas a vtable+8 (las vtables se referencian como lea reg,[vtable]).
# La instruccion 'lea reg,[rip+disp]' tiene opcode 48/4C 8D /r y disp32 = target - (rip).
# Es mas robusto escanear .text por 'lea' que apunte a VA = IB+0x16BDC68 o IB+0x16BE448.
# El helper find_pattern busca bytes. Calculamos: no podemos por disp variable.
# Usamos un escaneo manual del .text leyendo en bloques.
TEXT_START=0x1000
TEXT_END=0x1673000
targets={ IB+0x16BDC68:"Tasker_vt", IB+0x16BE448:"Melee_vt" }
# Tambien las vtables base de la jerarquia Task: escanear cualquier lea a rango 0x16BD000-0x16BF000
found=[]
CHUNK=0x40000
import sys
rva=TEXT_START
data=b''
# Leer todo el .text de una vez si el helper lo permite en trozos
buf=bytearray()
pos=TEXT_START
while pos<TEXT_END:
    n=min(CHUNK, TEXT_END-pos)
    b=k.bytes_at_rva(pos,n)
    if not b: break
    buf+=b
    pos+=n
print("text bytes leidos:", len(buf))
# Escanear lea: 48/4C 8D <modrm con mod=00,rm=101 (RIP)> disp32
i=0
L=len(buf)
hits=[]
while i < L-7:
    b0=buf[i]
    if b0 in (0x48,0x4C):
        if buf[i+1]==0x8D:
            modrm=buf[i+2]
            mod=(modrm>>6)&3; rm=modrm&7
            if mod==0 and rm==5:  # RIP-relative
                disp=struct.unpack('<i', buf[i+3:i+7])[0]
                insn_rva=TEXT_START+i
                next_rva=insn_rva+7
                target=IB+next_rva+disp
                if 0x16BD000 <= (target-IB) <= 0x16BFFFF:
                    hits.append((insn_rva, target-IB))
                    i+=7; continue
    i+=1
print("LEA a rango Task 0x16BD000-0x16BFFFF:", len(hits))
def rtti(vt):
    try:
        col=struct.unpack('<Q',k.bytes_at_rva(vt-8,8)[:8])[0]
        if col<IB: return None
        c=k.bytes_at_rva(col-IB,0x10); td=struct.unpack('<I',c[0xC:0x10])[0]
        return k.read_string_near(td+0x10,64)
    except: return None
for ins,tgt in hits:
    print(f"  lea@0x{ins:X} -> vt 0x{tgt:X} = {rtti(tgt)}")
