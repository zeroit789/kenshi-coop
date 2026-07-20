exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
import re
def readstr(rva,maxlen=200):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')

# Resolver vtable de una clase MyGUI por nombre de TD
def find_td(name):
    pat=name.encode('latin1')+b'\x00'
    for sec in ['.data','.rdata']:
        srva,ssize=secs[sec]
        blob=bytes(data[srva:srva+ssize])
        m=re.search(re.escape(pat),blob)
        if m:
            name_rva=srva+m.start(); return name_rva-16
    return None

def vtable_for_td(td_rva):
    # buscar COL en .rdata/.data cuyo +0xC (RVA) == td_rva
    for sec in ['.rdata','.data']:
        srva,ssize=secs[sec]
        for off in range(srva, srva+ssize-0x18,4):
            if u32(off+0xC)==td_rva:
                # validar: COL signature en +0x0 suele 0 o 1
                sig=u32(off)
                if sig in (0,1):
                    col_va=IB+off
                    # vtable: qword == col_va, vtable empieza +8
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
