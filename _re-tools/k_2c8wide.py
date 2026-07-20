exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)
OFF=0x2C8
STACK={Register.RBP,Register.RSP,Register.RIP}
def decode_func(b,e): return list(Decoder(64,data[b:e],ip=IB+b))

# Listar TODAS las funciones que LEEN [reg+0x2C8] (reg!=stack) como mov reg<-mem, sin exigir call.
# Para cada una reportamos contexto: que slots virtuales llama despues y si hay setVisible-like.
funcs={}
for (b,e,u) in PDATA:
    if not in_text(b): continue
    inss=decode_func(b,e)
    for i,ins in enumerate(inss):
        if ins.is_ip_rel_memory_operand: continue
        if ins.op_count>=2 and ins.op_kind(0)==OpKind.REGISTER and ins.op_kind(1)==OpKind.MEMORY \
           and ins.memory_displacement==OFF and ins.memory_base not in STACK and ins.memory_base!=Register.NONE \
           and ins.mnemonic==Mnemonic.MOV:
            funcs.setdefault(b,[]).append(ins.ip-IB)
print(f"funciones que LEEN [reg+0x2C8] como objeto: {len(funcs)}")
for fs in sorted(funcs):
    print(f"  {hex(fs)}: loads @ {[hex(x) for x in funcs[fs]]}")
