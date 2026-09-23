# ES: Confirma que [vtable+0x18] en un Widget de MyGUI es setVisible: localiza el TypeDescriptor
#     ".?AVWidget@MyGUI@@", su CompleteObjectLocator en .rdata, la vtable que apunta a ese COL y
#     vuelca sus 8 primeros slots. Carga k_setup.py/k_regs.py desde C:/Users/Zero/ktmp.
#     Uso: python k_setvis_confirm.py
# EN: Confirms that [vtable+0x18] on a MyGUI Widget is setVisible: finds the ".?AVWidget@MyGUI@@"
#     TypeDescriptor, its CompleteObjectLocator in .rdata, the vtable pointing to that COL and dumps
#     its first 8 slots. Loads k_setup.py/k_regs.py from C:/Users/Zero/ktmp.
#     Usage: python k_setvis_confirm.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)

# Confirmar que [vtable+0x18] sobre un Widget MyGUI es setVisible.
# El panel se crea con call 0x151A9 (en 0x72f7fe) y se guarda en this+0x2C8.
# 0x151A9 probablemente devuelve Widget*. Veamos su vtable: pero mejor, busquemos
# la vtable de Widget@MyGUI por RTTI y miremos el slot 0x18/8 = slot 3.
# EN: Confirm that [vtable+0x18] on a MyGUI Widget is setVisible.
#     The panel is created with call 0x151A9 (at 0x72f7fe) and stored at this+0x2C8.
#     0x151A9 probably returns Widget*. Let us look at its vtable: better, find
#     the Widget@MyGUI vtable through RTTI and look at slot 0x18/8 = slot 3.
import re
# ES: Lee una cadena terminada en 0 (latin1) desde un RVA.
# EN: Reads a NUL-terminated string (latin1) from an RVA.
def readstr(rva,maxlen=200):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')
# Buscar TD de Widget@MyGUI
# EN: Find the Widget@MyGUI TD
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
        # EN: find the COL pointing to this TD: COL+0xC == td_rva, in .rdata, and vtable = COL-...
        #     Simpler: find in .rdata a COL whose +0xC==td_rva, then the vtable is where vtable-8 points to that COL
        td_target=td_rva
        # escanear .rdata por u32==td_rva (campo pTypeDescriptor del COL)
        # EN: scan .rdata for u32==td_rva (pTypeDescriptor field of the COL)
        rrva,rsz=secs['.rdata']
        for off in range(rrva, rrva+rsz-0x18,4):
            if u32(off+0xC)==td_target and u32(off)in(0,1):
                col_rva=off
                col_va=IB+col_rva
                # buscar vtable: qword en .rdata == col_va, la vtable empieza 8 despues
                # EN: find the vtable: qword in .rdata == col_va, the vtable starts 8 bytes later
                for o2 in range((rrva+7)&~7, rrva+rsz-8,8):
                    if u64(o2)==col_va:
                        vt=o2+8
                        print(f"  COL@{hex(col_rva)} -> vtable Widget@{hex(vt)}")
                        # dump primeros 8 slots
                        # EN: dump the first 8 slots
                        for s in range(8):
                            q=u64(vt+s*8); r=q-IB
                            print(f"     slot[{s}] off+{hex(s*8)}: {hex(r) if in_text(r) else hex(q)}")
                        break
                break
        break
