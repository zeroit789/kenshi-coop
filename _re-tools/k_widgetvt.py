# ES: Para las clases MyGUI Widget, Button y TextBox: localiza su TypeDescriptor por nombre, el COL
#     que lo referencia y la vtable asociada, y vuelca los 10 primeros slots.
#     Carga k_setup.py desde C:/Users/Zero/ktmp. Uso: python k_widgetvt.py
# EN: For MyGUI classes Widget, Button and TextBox: finds their TypeDescriptor by name, the COL that
#     references it and the associated vtable, and dumps the first 10 slots.
#     Loads k_setup.py from C:/Users/Zero/ktmp. Usage: python k_widgetvt.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
import re
# ES: Lee una cadena terminada en 0 (latin1) desde un RVA.
# EN: Reads a NUL-terminated string (latin1) from an RVA.
def readstr(rva,maxlen=200):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')

# Resolver vtable de una clase MyGUI por nombre de TD
# EN: Resolve the vtable of a MyGUI class by TD name
def find_td(name):
    pat=name.encode('latin1')+b'\x00'
    for sec in ['.data','.rdata']:
        srva,ssize=secs[sec]
        blob=bytes(data[srva:srva+ssize])
        m=re.search(re.escape(pat),blob)
        if m:
            name_rva=srva+m.start(); return name_rva-16
    return None

# ES: Devuelve (vtable, COL) para un TypeDescriptor, o (None, None).
# EN: Returns (vtable, COL) for a TypeDescriptor, or (None, None).
def vtable_for_td(td_rva):
    # buscar COL en .rdata/.data cuyo +0xC (RVA) == td_rva
    # EN: find a COL in .rdata/.data whose +0xC (RVA) == td_rva
    for sec in ['.rdata','.data']:
        srva,ssize=secs[sec]
        for off in range(srva, srva+ssize-0x18,4):
            if u32(off+0xC)==td_rva:
                # validar: COL signature en +0x0 suele 0 o 1
                # EN: validate: COL signature at +0x0 is usually 0 or 1
                sig=u32(off)
                if sig in (0,1):
                    col_va=IB+off
                    # vtable: qword == col_va, vtable empieza +8
                    # EN: vtable: qword == col_va, the vtable starts at +8
                    for o2 in range((RDATA_RVA+7)&~7, RDATA_RVA+RDATA_SZ-8,8):
                        if u64(o2)==col_va:
                            return o2+8, off
    return None,None

for cls in ['.?AVWidget@MyGUI@@','.?AVButton@MyGUI@@','.?AVTextBox@MyGUI@@']:
    td=find_td(cls)
    if not td:
        print(cls,"TD no encontrado"); continue
    vt,col=vtable_for_td(td)
    print(f"\n{cls} TD@{hex(td)} COL@{hex(col) if col else '?'} vtable@{hex(vt) if vt else '?'}")
    if vt:
        for s in range(10):
            q=u64(vt+s*8); r=q-IB
            fc=func_containing(r) if in_text(r) else None
            print(f"   slot[{s}] off+{hex(s*8)}: {hex(r) if in_text(r) else hex(q)}")
