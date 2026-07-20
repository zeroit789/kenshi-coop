exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
import struct
def readstr(rva,n=160):
    b=bytearray();i=rva
    while i<len(data) and data[i]!=0 and len(b)<n: b.append(data[i]);i+=1
    return b.decode('latin1','replace')

def resolve_rtti(vt_rva):
    # vt-8 -> COL va -> rva -> +0xC pTD(RVA) -> name+0x10
    try:
        col_va=u64(vt_rva-8); col=col_va-IB
        if not in_rdata(col) and not in_data(col): return None
        td=u32(col+0xC)
        if not (in_rdata(td) or in_data(td)): return None
        nm=readstr(td+0x10)
        if nm.startswith('.?A'): return td,nm
    except: pass
    return None

# Escanear .rdata buscando vtables: un qword alineado V tal que vt-8 = COL valido y resolve da nombre.
# Pero eso es lento. Mejor: localizar el COL de Widget. Buscar COLs cuyo pTD==TD_Widget recorriendo
# todas las vtables (qwords en .rdata que apunten a .text en >=4 consecutivos).
rrva,rsz=secs['.rdata']
TD_WIDGET=0x1c28468
TD_BUTTON=0x1c28438
TD_TEXTBOX=0x1c36bb8
targets={TD_WIDGET:'Widget',TD_BUTTON:'Button',TD_TEXTBOX:'TextBox'}
found={}
o=rrva & ~7
end=rrva+rsz-8
while o<end:
    q=u64(o)
    r=q-IB
    # candidato a inicio de vtable: este slot apunta a .text y el anterior (o-8) apunta a un COL
    if in_text(r):
        rt=resolve_rtti(o)
        if rt:
            td,nm=rt
            if td in targets and td not in found:
                found[td]=(o,nm)
                print(f"VTABLE de {targets[td]} @ {hex(o)}  TD@{hex(td)} {nm}")
    o+=8
print("done. found:", {targets[k]:hex(v[0]) for k,v in found.items()})

# Dump de la vtable de Widget si la hallamos
def annotate(frva):
    fc=func_containing(frva)
    if not fc: return "(nopdata)"
    b,e=fc
    from iced_x86 import Decoder
    strs=[]
    for ins in Decoder(64,data[b:min(e,b+600)],ip=IB+b):
        if ins.is_ip_rel_memory_operand:
            t=ins.ip_rel_memory_address-IB
            if in_rdata(t):
                s=readstr(t,60)
                if s.isprintable() and 3<len(s)<55 and any(c.isalpha() for c in s): strs.append(s)
    return f"sz={e-b} {strs[:2]}"

for td,(vt,nm) in found.items():
    print(f"\n=== {targets[td]} vtable {hex(vt)} ===")
    for s in range(28):
        q=u64(vt+s*8); r=q-IB
        print(f"  [{s:2}] +{hex(s*8):>5}: {hex(r) if in_text(r) else hex(q)}  {annotate(r) if in_text(r) else ''}")
