exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register

fmt=Formatter(FormatterSyntax.INTEL)
def readstr(rva,maxlen=80):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')

b,e=func_containing(0x72D3B0)
# rdi = this (segun ctor). Buscar accesos a [rdi+0xB8] y [rdi+0xC0], y lea [rdi+X]
print("=== Accesos a offsets de interes en ctor MainBarGUI (rdi=this) ===")
for ins in Decoder(64,data[b:e],ip=IB+b):
    rva=ins.ip-IB
    txt=fmt.format(ins)
    # buscar displacement B8 o C0 con base rdi
    interesting=False
    if ins.memory_base==Register.RDI:
        d=ins.memory_displacement
        if d in (0xB8,0xC0,0xB0,0xC8):
            interesting=True
    # lea rX,[rdi+Y]
    if ins.mnemonic==Mnemonic.LEA and ins.memory_base==Register.RDI:
        interesting=True
    if interesting:
        print(f"  {hex(rva)}: {txt}")
