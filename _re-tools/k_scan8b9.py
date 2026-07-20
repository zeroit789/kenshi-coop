exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, OpKind, Mnemonic, MemorySize, Register
fmt=Formatter(FormatterSyntax.INTEL)
text=data[TEXT_RVA:TEXT_RVA+TEXT_SZ]
dec=Decoder(64,text,ip=IB+TEXT_RVA)
# Buscar cualquier instruccion con desplazamiento de memoria == 0x8B9
hits=[]
for ins in dec:
    if ins.memory_displacement & 0xFFFFFFFF == 0x8B9 and ins.memory_base not in (Register.RIP, Register.NONE):
        rva=ins.ip-IB
        fc=func_containing(rva)
        hits.append((rva, rn(ins.memory_base), str(ins.mnemonic), fmt.format(ins), hex(fc[0]) if fc else '?'))
print(f"Accesos a [reg+0x8B9] en .text: {len(hits)}")
for rva,base,mn,txt,fn in hits:
    print(f"  RVA {hex(rva)} func={fn:12} base={base:5} {txt}")
