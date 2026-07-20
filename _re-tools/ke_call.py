# resuelve target de call/jmp rel32 dado RVA del call
import sys, ke_dis
img = ke_dis._image
ke_dis.load()
img = ke_dis._image
for a in sys.argv[1:]:
    rva=int(a,16)
    op=img[rva]
    if op in (0xE8,0xE9):
        import struct
        rel=struct.unpack_from("<i",img,rva+1)[0]
        tgt=rva+5+rel
        # si es thunk jmp, resolver 1 nivel
        kind="call" if op==0xE8 else "jmp"
        extra=""
        if img[tgt]==0xE9:
            rel2=struct.unpack_from("<i",img,tgt+1)[0]
            extra=f"  -> thunk jmp -> 0x{tgt+5+rel2:X}"
        elif img[tgt]==0xFF and img[tgt+1]==0x25:
            extra="  (iat jmp)"
        print(f"0x{rva:X} {kind} -> 0x{tgt:X}{extra}")
    else:
        print(f"0x{rva:X} no es call/jmp rel32 (op {op:02X})")
