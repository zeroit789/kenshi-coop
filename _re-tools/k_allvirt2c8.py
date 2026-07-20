exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)
OFF=0x2C8
STACK={Register.RBP,Register.RSP,Register.RIP}
def decode_func(b,e): return list(Decoder(64,data[b:e],ip=IB+b))
res=[]
for (b,e,u) in PDATA:
    if not in_text(b): continue
    inss=decode_func(b,e)
    for i,ins in enumerate(inss):
        if ins.is_ip_rel_memory_operand or ins.mnemonic!=Mnemonic.MOV: continue
        if ins.op_count==2 and ins.op_kind(0)==OpKind.REGISTER and ins.op_kind(1)==OpKind.MEMORY \
           and ins.memory_displacement==OFF and ins.memory_base not in STACK and ins.memory_base!=Register.NONE:
            objreg=ins.op_register(0)
            vtregs={objreg}
            for nx in inss[i+1:i+12]:
                if nx.mnemonic==Mnemonic.MOV and nx.op_count==2 and nx.op_kind(0)==OpKind.REGISTER and nx.op_kind(1)==OpKind.MEMORY and nx.memory_base in vtregs and nx.memory_displacement==0 and not nx.is_ip_rel_memory_operand:
                    vtregs.add(nx.op_register(0))
                if nx.mnemonic==Mnemonic.CALL and nx.op_count==1 and nx.op_kind(0)==OpKind.MEMORY and nx.memory_base in vtregs and not nx.is_ip_rel_memory_operand:
                    res.append((b, ins.ip-IB, nx.ip-IB, nx.memory_displacement))
                    break
print(f"funcs con [reg+0x2C8]->call virtual: {len(res)}")
for fs,lr,cr,slot in res:
    print(f"  func {hex(fs)}  load@{hex(lr)} call[vt+{hex(slot)}]@{hex(cr)}")
