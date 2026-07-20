exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)

# Confirmar que [vtable+0x18] sobre un Widget MyGUI es setVisible.
# El panel se crea con call 0x151A9 (en 0x72f7fe) y se guarda en this+0x2C8.
# 0x151A9 probablemente devuelve Widget*. Veamos su vtable: pero mejor, busquemos
# la vtable de Widget@MyGUI por RTTI y miremos el slot 0x18/8 = slot 3.
import re
def readstr(rva,maxlen=200):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')
# Buscar TD de Widget@MyGUI
blob_off=None
for sec in ['.data','.rdata']:
    srva,ssize=secs[sec]
    blob=bytes(data[srva:srva+ssize])
    m=re.search(rb'\.\?AVWidget@MyGUI@@\x00',blob)
    if m:
        name_rva=srva+m.start(); td_rva=name_rva-16
        print(f"Widget@MyGUI TD @ {hex(td_rva)}")
        # buscar COL que apunte a este TD: COL+0xC == td_rva, en .rdata, y vtable = COL-... 
        # Mas simple: buscar en .rdata un COL cuyo +0xC==td_rva, luego vtable es donde vtable-8 apunta a ese COL
        td_target=td_rva
        # escanear .rdata por u32==td_rva (campo pTypeDescriptor del COL)
        rrva,rsz=secs['.rdata']
        for off in range(rrva, rrva+rsz-0x18,4):
            if u32(off+0xC)==td_target and u32(off)in(0,1):
                col_rva=off
                col_va=IB+col_rva
                # buscar vtable: qword en .rdata == col_va, la vtable empieza 8 despues
                for o2 in range((rrva+7)&~7, rrva+rsz-8,8):
                    if u64(o2)==col_va:
                        vt=o2+8
                        print(f"  COL@{hex(col_rva)} -> vtable Widget@{hex(vt)}")
                        # dump primeros 8 slots
                        for s in range(8):
                            q=u64(vt+s*8); r=q-IB
                            print(f"     slot[{s}] off+{hex(s*8)}: {hex(r) if in_text(r) else hex(q)}")
                        break
                break
        break
