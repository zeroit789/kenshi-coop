# ES: Para cada RVA de un call/jmp rel32 (E8/E9) pasado por argumento, calcula el destino y, si el
#     destino es un thunk jmp o un jmp de IAT (FF 25), lo indica. Usa ke_dis.py (imagen mapeada).
#     Uso: python ke_call.py <rva_hex> [...]
# EN: For each rel32 call/jmp RVA (E8/E9) passed as argument, computes the target and, if the target
#     is a jmp thunk or an IAT jmp (FF 25), says so. Uses ke_dis.py (mapped image).
#     Usage: python ke_call.py <rva_hex> [...]

# resuelve target de call/jmp rel32 dado RVA del call
# EN: resolves the target of a rel32 call/jmp given the call RVA
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
        # EN: if it is a jmp thunk, resolve 1 level
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
