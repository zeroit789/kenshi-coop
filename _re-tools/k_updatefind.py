exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
import iced_x86
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)
OFF=0x2C8
STACKREGS={Register.RBP,Register.RSP,Register.RIP}
def regname(r):
    return iced_x86.register_to_string(r) if hasattr(iced_x86,'register_to_string') else str(r)
def decode_func(b_rva,e_rva):
    return list(Decoder(64,data[b_rva:e_rva],ip=IB+b_rva))
cands=[]
for (b,e,u) in PDATA:
    if not in_text(b): continue
    inss=decode_func(b,e)
    for i,ins in enumerate(inss):
        if ins.is_ip_rel_memory_operand: continue
        if ins.mnemonic!=Mnemonic.MOV: continue
        if ins.op_count==2 and ins.op_kind(0)==OpKind.REGISTER and ins.op_kind(1)==OpKind.MEMORY:
            if ins.memory_displacement==OFF and ins.memory_base not in STACKREGS and ins.memory_base!=Register.NONE:
                loaded=ins.op_register(0)
                window=inss[i+1:i+9]
                ok=False; detail=""; cur=loaded
                for nx in window:
                    if nx.mnemonic==Mnemonic.MOV and nx.op_count==2 and nx.op_kind(0)==OpKind.REGISTER and nx.op_kind(1)==OpKind.MEMORY and nx.memory_base==cur and nx.memory_displacement==0 and not nx.is_ip_rel_memory_operand:
                        cur=nx.op_register(0)
                    if nx.mnemonic==Mnemonic.CALL and nx.op_count==1 and nx.op_kind(0)==OpKind.MEMORY and nx.memory_base==cur and not nx.is_ip_rel_memory_operand:
                        ok=True; detail=f"call [{regname(cur)}+{hex(nx.memory_displacement)}] @ {hex(nx.ip-IB)}"
                        break
                if ok:
                    cands.append((b, ins.ip-IB, detail))
print(f"candidatos (load [reg+0x2C8] -> call virtual): {len(cands)}")
for fstart,rva,det in cands:
    print(f"  func {hex(fstart)}  @ {hex(rva)}  {det}")
