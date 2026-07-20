import pefile
PATH=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(PATH, fast_load=True)
secs=[(s.Name.rstrip(b'\x00').decode('latin1'),s.VirtualAddress,s.Misc_VirtualSize) for s in pe.sections]
target=0x7874E0
hits=[]
for name,va,vs in secs:
    if name!='.text':continue
    data=pe.get_data(va,vs);n=len(data);i=0
    while i<n-5:
        if data[i]==0xE9:
            rel=int.from_bytes(data[i+1:i+5],'little',signed=True)
            tgt=(va+i+5+rel)&0xFFFFFFFF
            if tgt==target:
                hits.append(va+i)
        i+=1
for h in hits: print(f"JMP thunk @0x{h:X} -> 0x{target:X}")
print("thunks found:",len(hits))
