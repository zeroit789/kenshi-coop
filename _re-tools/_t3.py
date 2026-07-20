import pefile
PATH=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
pe=pefile.PE(PATH, fast_load=True)
secs=[(s.Name.rstrip(b'\x00').decode('latin1'),s.VirtualAddress,s.Misc_VirtualSize) for s in pe.sections]
def thunks(target):
  hits=[]
  for name,va,vs in secs:
    if name!='.text':continue
    data=pe.get_data(va,vs);n=len(data);i=0
    while i<n-5:
        if data[i]==0xE9:
            rel=int.from_bytes(data[i+1:i+5],'little',signed=True)
            tgt=(va+i+5+rel)&0xFFFFFFFF
            if tgt==target: hits.append(va+i)
        i+=1
  return hits
print("thunks to 0x623920:",[hex(h) for h in thunks(0x623920)])
