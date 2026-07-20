import ke_re as k
import struct
IMG=0x140000000
def u64(b): return struct.unpack("<Q",b)[0]
def vt_slots(vt, n=20):
    out=[]
    for i in range(n):
        va=u64(bytes(k.bytes_at_rva(vt+i*8,8)))
        if not (IMG <= va <= IMG+0x1671412):
            out.append(None); continue
        rva=va-IMG
        try:
            b=bytes(k.bytes_at_rva(rva,5))
        except Exception:
            out.append(rva); continue
        if b[0]==0xE9:
            disp=struct.unpack("<i",b[1:5])[0]; rva=rva+5+disp
        out.append(rva)
    return out

for name,vt in [("AppearanceHuman",0x16E6338),("AppearanceAnimal",0x16E6598)]:
    sl=vt_slots(vt,20)
    print(name, "contiene 0x5bd240?", "SI" if 0x5bd240 in sl else "NO")

base=0x5C0000
data=bytes(k.bytes_at_rva(base, 0x30000))
REX={0x48,0x4C,0x49,0x4D}
print("\n--- escrituras mov [reg+0xE8],reg en 0x5C0000-0x5F0000 ---")
for i in range(len(data)-7):
    if data[i] in REX and data[i+1]==0x89:
        m=data[i+2]; mod=(m>>6)&3; rm=m&7
        if mod==2 and rm!=4 and rm!=5 and data[i+3]==0xE8 and data[i+4]==0 and data[i+5]==0 and data[i+6]==0:
            print("  W", hex(base+i))
