# ES: Busca en .text las instrucciones "lea reg, [rip+disp32]" (48/4C 8D, modrm RIP-relativo) cuyo
#     destino es la vtable 0x16F9EB8, es decir, el código que carga esa vtable (constructores).
#     Uso: python _xref2.py
# EN: Searches .text for "lea reg, [rip+disp32]" instructions (48/4C 8D, RIP-relative modrm) whose
#     target is vtable 0x16F9EB8, i.e. the code that loads that vtable (constructors).
#     Usage: python _xref2.py

import ke_re as k, struct
pe,data=k._load()
text=[s for s in pe.sections if b".text" in s.Name][0]
start=text.PointerToRawData; end=start+text.SizeOfRawData; base=text.VirtualAddress
# ES: Devuelve los RVAs de cada lea RIP-relativo que apunta a target.
# EN: Returns the RVAs of every RIP-relative lea pointing to target.
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
