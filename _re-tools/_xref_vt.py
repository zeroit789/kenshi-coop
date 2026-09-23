# ES: Lista las referencias "lea reg, [rip+disp32]" a la vtable 0x16F4588 (AnimationClassAnimal)
#     en .text, con sus 7 bytes. Sirve para encontrar el constructor que instala esa vtable.
#     Uso: python _xref_vt.py
# EN: Lists "lea reg, [rip+disp32]" references to vtable 0x16F4588 (AnimationClassAnimal)
#     in .text, with their 7 bytes. Used to find the constructor that installs that vtable.
#     Usage: python _xref_vt.py

import ke_re as k
pe,data=k._load()
text=[s for s in pe.sections if b".text" in s.Name][0]
start=text.PointerToRawData; end=start+text.SizeOfRawData; base=text.VirtualAddress
IMG=0x140000000
target=0x16F4588  # vtable AnimationClassAnimal (RVA)
# lea reg,[rip+disp] -> REX.W 8D /r mod=00 rm=101 ; opcode 8D, modrm bits
res=[]
# ES: Recorrido byte a byte buscando la codificación del lea RIP-relativo.
# EN: Byte-by-byte walk looking for the RIP-relative lea encoding.
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
