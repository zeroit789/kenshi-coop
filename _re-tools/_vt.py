import pefile
PATH=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(PATH, fast_load=True)
secs=[(s.Name.rstrip(b'\x00').decode('latin1'),s.VirtualAddress,s.Misc_VirtualSize) for s in pe.sections]
for tgt in (0x623920, 0x477bc):
  needle=(IB+tgt).to_bytes(8,'little')
  for name,va,vs in secs:
    data=pe.get_data(va,vs); i=data.find(needle)
    while i!=-1:
        print(f"ptr to 0x{tgt:X} in {name} @0x{va+i:X}")
        i=data.find(needle,i+1)
