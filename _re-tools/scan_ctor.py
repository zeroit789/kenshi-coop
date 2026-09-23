# ES: En el constructor de MainBarGUI (función que contiene 0x72D3B0, rdi = this) lista los accesos a
#     [rdi+0xB0/0xB8/0xC0/0xC8] y los lea [rdi+X]. Carga k_setup.py/k_regs.py desde C:/Users/Zero/ktmp.
#     Uso: python scan_ctor.py
# EN: In the MainBarGUI constructor (function containing 0x72D3B0, rdi = this) lists the accesses to
#     [rdi+0xB0/0xB8/0xC0/0xC8] and the lea [rdi+X]. Loads k_setup.py/k_regs.py from C:/Users/Zero/ktmp.
#     Usage: python scan_ctor.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register

# ES: Formateador Intel con prefijo 0x y mapa valor -> nombre de registro (RN).
# EN: Intel formatter with 0x prefix and value -> register name map (RN).
fmt=Formatter(FormatterSyntax.INTEL)
# ES: Lee una cadena terminada en 0 (latin1) desde un RVA (no se usa aquí).
# EN: Reads a NUL-terminated string (latin1) from an RVA (unused here).
def readstr(rva,maxlen=80):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')

b,e=func_containing(0x72D3B0)
# rdi = this (segun ctor). Buscar accesos a [rdi+0xB8] y [rdi+0xC0], y lea [rdi+X]
# EN: rdi = this (per the ctor). Look for accesses to [rdi+0xB8] and [rdi+0xC0], and lea [rdi+X]
print("=== Accesos a offsets de interes en ctor MainBarGUI (rdi=this) ===")
for ins in Decoder(64,data[b:e],ip=IB+b):
    rva=ins.ip-IB
    txt=fmt.format(ins)
    # buscar displacement B8 o C0 con base rdi
    # EN: look for displacement B8 or C0 with base rdi (also B0/C8)
    interesting=False
    if ins.memory_base==Register.RDI:
        d=ins.memory_displacement
        if d in (0xB8,0xC0,0xB0,0xC8):
            interesting=True
    # lea rX,[rdi+Y]
    # EN: lea rX,[rdi+Y]
    if ins.mnemonic==Mnemonic.LEA and ins.memory_base==Register.RDI:
        interesting=True
    if interesting:
        print(f"  {hex(rva)}: {txt}")
