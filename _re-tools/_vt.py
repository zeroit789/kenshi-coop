# ES: Busca en todas las secciones punteros absolutos (8 bytes, base 0x140000000) a 0x623920 y al
#     thunk 0x477BC, para localizar vtables u otras tablas que los referencian.
#     Uso: python _vt.py
# EN: Searches every section for absolute pointers (8 bytes, base 0x140000000) to 0x623920 and to
#     thunk 0x477BC, to locate vtables or other tables that reference them.
#     Usage: python _vt.py

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
