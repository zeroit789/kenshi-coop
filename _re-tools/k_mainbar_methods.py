# ES: Busca las funciones que referencian (lea RIP-relativo) la vtable de MainBarGUI o su subobjeto
#     BaseLayout, y puntúa 17 candidatas (las que leen +0x2C8) según cuántos campos característicos
#     del HUD tocan. Carga k_setup.py/k_regs.py desde C:/Users/Zero/ktmp. Uso: python k_mainbar_methods.py
# EN: Finds the functions that reference (RIP-relative lea) the MainBarGUI vtable or its BaseLayout
#     subobject, and scores 17 candidates (those reading +0x2C8) by how many characteristic HUD fields
#     they touch. Loads k_setup.py/k_regs.py from C:/Users/Zero/ktmp. Usage: python k_mainbar_methods.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Mnemonic, OpKind, Register

VT_MAIN=0x17099f8
# ES: vtable MainBarGUI y subobjeto BaseLayout (RVAs)
# EN: MainBarGUI vtable and BaseLayout subobject (RVAs)
VT_BASE=0x168ceb8  # BaseLayout subobjeto
vt_targets={IB+VT_MAIN, IB+VT_BASE}

# Funciones que referencian la vtable de MainBarGUI por rip-rel (lea) -> son ctor/metodos/factory
# EN: Functions referencing the MainBarGUI vtable via rip-rel (lea) -> ctor/methods/factory
text_bytes=data[TEXT_RVA:TEXT_RVA+TEXT_SZ]
dec=Decoder(64,text_bytes,ip=IB+TEXT_RVA)
refs=set()
for ins in dec:
    if ins.is_ip_rel_memory_operand and ins.ip_rel_memory_address in vt_targets:
        fc=func_containing(ins.ip-IB)
        if fc: refs.add(fc[0])
print("funcs que referencian vtable MainBarGUI/BaseLayout:", [hex(x) for x in sorted(refs)])

# El conjunto de campos-widget caracteristicos del HUD (del ctor)
# EN: The set of characteristic HUD widget fields (from the ctor)
HUD_FIELDS={0xe0,0xe8,0x1e8,0x240,0x280,0x2b8,0x2c8}
def decode_func(b,e): return list(Decoder(64,data[b:e],ip=IB+b))
# ES: Cuántos de los campos HUD_FIELDS toca la función que contiene fs (y cuáles).
# EN: How many of the HUD_FIELDS the function containing fs touches (and which ones).
def field_score(fs):
    fc=func_containing(fs)
    if not fc: return 0,set()
    b,e=fc
    touched=set()
    for ins in decode_func(b,e):
        if ins.is_ip_rel_memory_operand: continue
        for opi in range(ins.op_count):
            if ins.op_kind(opi)==OpKind.MEMORY and ins.memory_base not in (Register.RBP,Register.RSP,Register.RIP) and ins.memory_base!=Register.NONE:
                d=ins.memory_displacement
                if d in HUD_FIELDS: touched.add(d)
    return len(touched),touched

# Las 17 que leen +0x2C8
# EN: The 17 that read +0x2C8
cands=[0x44bf40,0x494af0,0x49bf90,0x49bfbb,0x49bff2,0x49f5b0,0x4cc620,0x510920,0x5547f0,0x5609e0,0x562a8f,0x5eaa30,0x5ebd50,0x5f1764,0x674a84,0x680560,0x72d3b0]
print("\nscore de campos-HUD por candidato (lee +0x2C8):")
for fs in cands:
    sc,ts=field_score(fs)
    mark=" <== refs vtable" if fs in refs else ""
    print(f"  {hex(fs)}: score={sc} fields={sorted(hex(x) for x in ts)}{mark}")
