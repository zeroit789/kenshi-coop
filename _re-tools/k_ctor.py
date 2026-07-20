exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
import iced_x86
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)
def decode_func(b,e): return list(Decoder(64,data[b:e],ip=IB+b))

# El ctor de la clase HUD: instala vtable (lea rax,[rip vtable]; mov [rcx],rax)
# y referencia offset +0x2C8 o +0x2B8. Buscamos funciones que:
#  (a) tengan lea reg,[rip->rdata] seguido de mov [base+0],reg  (vtable install en this+0)
#  (b) toquen displacement 0x2B8 o 0x2C8
# Reportamos la vtable instalada.
hits=[]
for (b,e,u) in PDATA:
    if not in_text(b): continue
    inss=decode_func(b,e)
    vtable_installs=[]
    touches=False
    for i,ins in enumerate(inss):
        if ins.mnemonic==Mnemonic.LEA and ins.is_ip_rel_memory_operand:
            tgt=ins.ip_rel_memory_address-IB
            if in_rdata(tgt):
                reg=ins.op_register(0)
                # siguiente mov [base+disp], reg
                for nx in inss[i+1:i+4]:
                    if nx.mnemonic==Mnemonic.MOV and nx.op_count==2 and nx.op_kind(0)==OpKind.MEMORY and nx.op_kind(1)==OpKind.REGISTER and nx.op_register(1)==reg and not nx.is_ip_rel_memory_operand:
                        vtable_installs.append((nx.ip-IB,tgt,nx.memory_displacement))
        for opi in range(ins.op_count):
            if ins.op_kind(opi)==OpKind.MEMORY and ins.memory_displacement in (0x2B8,0x2C8) and ins.memory_base not in (Register.RBP,Register.RSP,Register.RIP):
                touches=True
    if vtable_installs and touches:
        hits.append((b,vtable_installs))
print(f"posibles ctors (vtable install + toca 2B8/2C8): {len(hits)}")
for fs,vs in hits:
    print(f"\n--- func {hex(fs)} ---")
    for rva,vt,disp in vs:
        print(f"    install vtable {hex(vt)} en this+{hex(disp)} @ {hex(rva)}")
