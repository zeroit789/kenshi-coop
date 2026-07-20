exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
import iced_x86
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)
def regname(r): return iced_x86.register_to_string(r)
OFF=0x2C8
STACKREGS={Register.RBP,Register.RSP,Register.RIP}
def decode_func(b,e): return list(Decoder(64,data[b:e],ip=IB+b))

# Mas laxo: cualquier funcion que cargue [base+0x2C8] (base!=stack) y dentro de 14 ins
# haga un call qword [reg+0x18] donde reg provenga de deref de ese puntero.
cands=[]
for (b,e,u) in PDATA:
    if not in_text(b): continue
    inss=decode_func(b,e)
    for i,ins in enumerate(inss):
        if ins.is_ip_rel_memory_operand or ins.mnemonic!=Mnemonic.MOV: continue
        if ins.op_count==2 and ins.op_kind(0)==OpKind.REGISTER and ins.op_kind(1)==OpKind.MEMORY \
           and ins.memory_displacement==OFF and ins.memory_base not in STACKREGS and ins.memory_base!=Register.NONE:
            objreg=ins.op_register(0)
            # rastrear vtable deref: reg que contiene vtable
            vtregs=set()
            for nx in inss[i+1:i+16]:
                if nx.mnemonic==Mnemonic.MOV and nx.op_count==2 and nx.op_kind(0)==OpKind.REGISTER and nx.op_kind(1)==OpKind.MEMORY \
                   and nx.memory_base==objreg and nx.memory_displacement==0 and not nx.is_ip_rel_memory_operand:
                    vtregs.add(nx.op_register(0))
                if nx.mnemonic==Mnemonic.CALL and nx.op_count==1 and nx.op_kind(0)==OpKind.MEMORY \
                   and nx.memory_base in vtregs and nx.memory_displacement==0x18 and not nx.is_ip_rel_memory_operand:
                    cands.append((b, ins.ip-IB, nx.ip-IB))
                    break
print(f"funciones con load[reg+0x2C8] + call[vt+0x18]: {len(cands)}")
for fs,lr,cr in cands:
    print(f"  func {hex(fs)}  load@{hex(lr)} call@{hex(cr)}")
