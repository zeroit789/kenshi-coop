# ES: Lee tres vtables de .rdata e imprime qué función hay en los slots +0x228 (init),
#     +0x378 (giveBirth), +0x80 (registro) y +0xE8 (tick de IA). Nombres de slot según notas de RE.
#     Uso: python _slots.py
# EN: Reads three vtables from .rdata and prints which function sits at slots +0x228 (init),
#     +0x378 (giveBirth), +0x80 (registration) and +0xE8 (AI tick). Slot names come from RE notes.
#     Usage: python _slots.py

import pefile
PATH=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(PATH, fast_load=True)
rd=[s for s in pe.sections if s.Name.rstrip(b'\x00')==b'.rdata'][0]
rdva=rd.VirtualAddress; rdata=pe.get_data(rdva,rd.Misc_VirtualSize)
# ES: Devuelve el RVA de la función guardada en vtable+off.
# EN: Returns the RVA of the function stored at vtable+off.
def slot(vt, off):
    a=vt-rdva+off
    return int.from_bytes(rdata[a:a+8],'little')-IB
for vt in (0x16F2208,0x16F2848,0x16F9EB8):
    print(f"vtable 0x{vt:X}: slot+0x228(init)=0x{slot(vt,0x228):X}  slot+0x378(giveBirth)=0x{slot(vt,0x378):X}  slot+0x80(reg)=0x{slot(vt,0x80):X}  slot+0xE8(AItick)=0x{slot(vt,0xE8):X}")
