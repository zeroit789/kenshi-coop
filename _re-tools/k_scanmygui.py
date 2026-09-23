# ES: Recorre .rdata buscando vtables cuyo nombre RTTI contenga "Widget@MyGUI" (Widget de MyGUI y
#     clases cuyo nombre lo incluya) e imprime hasta 40. Carga k_setup.py desde C:/Users/Zero/ktmp.
#     Uso: python k_scanmygui.py
# EN: Walks .rdata looking for vtables whose RTTI name contains "Widget@MyGUI" (MyGUI Widget and
#     classes whose name includes it) and prints up to 40. Loads k_setup.py from C:/Users/Zero/ktmp.
#     Usage: python k_scanmygui.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
# ES: Lee una cadena terminada en 0 (latin1) desde un RVA.
# EN: Reads a NUL-terminated string (latin1) from an RVA.
def readstr(rva,n=200):
    b=bytearray();i=rva
    while i<len(data) and data[i]!=0 and len(b)<n: b.append(data[i]);i+=1
    return b.decode('latin1','replace')
# ES: (TypeDescriptor, nombre) de una vtable vía su CompleteObjectLocator, o None.
# EN: (TypeDescriptor, name) of a vtable through its CompleteObjectLocator, or None.
def resolve_rtti(vt_rva):
    try:
        col_va=u64(vt_rva-8); col=col_va-IB
        if not (in_rdata(col) or in_data(col)): return None
        td=u32(col+0xC)
        if not (in_rdata(td) or in_data(td)): return None
        nm=readstr(td+0x10)
        if nm.startswith('.?A'): return td,nm
    except: return None
    return None

# Escanear TODO .rdata por vtables y volcar las que el nombre contenga 'Widget@MyGUI' exacto como clase mas derivada,
# o cualquier subclase directa de Widget. Recogemos todas las vtables MyGUI.
# EN: Scan ALL of .rdata for vtables and dump those whose name contains exactly 'Widget@MyGUI' as the
#     most derived class, or any direct Widget subclass. We collect all MyGUI vtables.
rrva,rsz=secs['.rdata']
o=rrva & ~7; end=rrva+rsz-8
hits=[]
while o<end:
    q=u64(o); r=q-IB
    if in_text(r):
        rt=resolve_rtti(o)
        if rt:
            td,nm=rt
            if 'Widget@MyGUI' in nm or nm=='.?AVWidget@MyGUI@@':
                hits.append((o,td,nm))
    o+=8
print(f"vtables cuyo TD-name contiene 'Widget@MyGUI': {len(hits)}")
for vt,td,nm in hits[:40]:
    print(f"  vt@{hex(vt)} TD@{hex(td)} {nm}")
