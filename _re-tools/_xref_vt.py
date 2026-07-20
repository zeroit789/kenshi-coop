import ke_re as k
pe,data=k._load()
text=[s for s in pe.sections if b".text" in s.Name][0]
start=text.PointerToRawData; end=start+text.SizeOfRawData; base=text.VirtualAddress
IMG=0x140000000
target=0x16F4588  # vtable AnimationClassAnimal (RVA)
# lea reg,[rip+disp] -> REX.W 8D /r mod=00 rm=101 ; opcode 8D, modrm bits
res=[]
i=start
while i<end-7:
    b0=data[i]
    if b0 in (0x48,0x4C) and data[i+1]==0x8D:
        modrm=data[i+2]
        if (modrm&0xC7)==0x05:  # mod=00, rm=101 => RIP-relative
            import struct
            disp=struct.unpack('<i',data[i+3:i+7])[0]
            instr_end_rva=base+(i-start)+7
            tgt=instr_end_rva+disp
            if tgt==target:
                rva=base+(i-start)
                res.append(rva)
    i+=1
print("xrefs a vtable 0x16F4588:",len(res))
for r in res:
    print(hex(r), k.hexbytes(r,7))
