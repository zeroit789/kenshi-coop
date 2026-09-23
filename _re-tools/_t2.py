# ES: Busca en .text todos los thunks "jmp rel32" (opcode E9) que saltan a la función 0x796C40.
#     Uso: python _t2.py. Imprime la lista de RVAs de los thunks.
# EN: Searches .text for every "jmp rel32" thunk (opcode E9) that jumps to function 0x796C40.
#     Usage: python _t2.py. Prints the list of thunk RVAs.

import pefile
PATH=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
pe=pefile.PE(PATH, fast_load=True)
secs=[(s.Name.rstrip(b'\x00').decode('latin1'),s.VirtualAddress,s.Misc_VirtualSize) for s in pe.sections]
for target in (0x796C40,):
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
  print(f"target 0x{target:X} thunks:",[hex(h) for h in hits])
