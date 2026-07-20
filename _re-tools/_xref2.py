import ke_re as k, struct
pe,data=k._load()
text=[s for s in pe.sections if b".text" in s.Name][0]
start=text.PointerToRawData; end=start+text.SizeOfRawData; base=text.VirtualAddress
def xrefs(target):
    res=[]; i=start
    while i<end-7:
        if data[i] in (0x48,0x4C) and data[i+1]==0x8D:
            modrm=data[i+2]
            if (modrm&0xC7)==0x05:
                disp=struct.unpack('<i',data[i+3:i+7])[0]
                tgt=base+(i-start)+7+disp
                if tgt==target: res.append(base+(i-start))
        i+=1
    return res
for vt in (0x16F9EB8,):
    r=xrefs(vt); print('vtable',hex(vt),'xrefs:',[hex(x) for x in r])
