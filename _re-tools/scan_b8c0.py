exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Mnemonic, OpKind, Register

# Escanear cada funcion de PDATA dentro de .text. Para cada una:
#  - marcar si lee/escribe desplazamiento 0xB8 y 0xC0 (cualquier base reg que no sea rsp/rbp)
#  - marcar si hace un call qword [reg+0x18] (posible setVisible)
#  - marcar si lee GameWorld+0x8B9 (acceso a 0x2134110 + ... no, es abs) o referencia .data 0x2134110
GW=0x2134110
res=[]
for (b,e,u) in PDATA:
    if not (TEXT_RVA<=b<TEXT_RVA+TEXT_SZ): continue
    has_b8=False; has_c0=False; call18=False; refgw=False; call88=False
    try:
        dec=Decoder(64,data[b:e],ip=IB+b)
    except Exception:
        continue
    for ins in dec:
        mb=ins.memory_base
        if mb not in (Register.RSP,Register.RBP,Register.NONE):
            d=ins.memory_displacement
            if d==0xB8: has_b8=True
            elif d==0xC0: has_c0=True
        if ins.mnemonic==Mnemonic.CALL and ins.op0_kind==OpKind.MEMORY:
            if ins.memory_base not in (Register.RSP,Register.RBP,Register.NONE) and ins.memory_index==Register.NONE:
                d=ins.memory_displacement
                if d==0x18: call18=True
                if d==0x88: call88=True
        if ins.is_ip_rel_memory_operand:
            t=ins.ip_rel_memory_address-IB
            if t==GW: refgw=True
    if has_b8 and has_c0:
        res.append((b,e,has_b8,has_c0,call18,call88,refgw))

print(f"Funcs con acceso a +0xB8 Y +0xC0: {len(res)}")
for (b,e,b8,c0,c18,c88,gw) in res:
    flags=[]
    if c18: flags.append("call[+0x18]=setVisible?")
    if c88: flags.append("call[+0x88]=setCaption?")
    if gw: flags.append("refGameWorld")
    print(f"  func {hex(b)}..{hex(e)} size {e-b}  {' '.join(flags)}")
