exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, OpKind, Mnemonic, Register
fmt=Formatter(FormatterSyntax.INTEL)
text=data[TEXT_RVA:TEXT_RVA+TEXT_SZ]
import sys
OFF=int(sys.argv[1],16)
dec=Decoder(64,text,ip=IB+TEXT_RVA)
hits=[]
for ins in dec:
    if (ins.memory_displacement & 0xFFFFFFFF)==OFF and ins.memory_base not in (Register.RIP,Register.NONE):
        # solo escrituras (mov [mem],reg) -> op0 es memoria
        if ins.mnemonic==Mnemonic.MOV and ins.op_count>=2 and ins.op_kind(0)==OpKind.MEMORY:
            rva=ins.ip-IB; fc=func_containing(rva)
            hits.append((rva,rn(ins.memory_base),fmt.format(ins),hex(fc[0]) if fc else '?'))
print(f"escrituras a [reg+{hex(OFF)}]: {len(hits)}")
for rva,base,txt,fn in hits: print(f"  RVA {hex(rva)} func={fn:12} {txt}")
