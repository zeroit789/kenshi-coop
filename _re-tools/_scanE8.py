import ke_re as k
pe,data=k._load()
text=[s for s in pe.sections if b".text" in s.Name][0]
start=text.PointerToRawData; end=start+text.SizeOfRawData; base=text.VirtualAddress
# Buscar mov [reg+0xE8], reg  (disp32 = E8 00 00 00) sin SIB, REX.W
disp=bytes([0xE8,0x00,0x00,0x00])
res=[]
i=start
while i<end-7:
    if data[i] in (0x48,0x4C,0x49,0x4D) and data[i+1]==0x89:
        modrm=data[i+2]; mod=modrm>>6; rm=modrm&7
        if mod==0b10 and rm!=0b100 and rm!=0b101:
            if data[i+3:i+7]==disp:
                rva=base+(i-start)
                res.append(rva)
    i+=1
print(len(res),"escrituras mov [reg+0xE8],reg")
for r in res:
    print(hex(r), k.hexbytes(r,7))
