exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Mnemonic, OpKind, Register
def readstr(rva,maxlen=200):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')
b,e=func_containing(0x72d3b0)
inss=list(Decoder(64,data[b:e],ip=IB+b))
installs=[]
for i,ins in enumerate(inss):
    if ins.mnemonic==Mnemonic.LEA and ins.is_ip_rel_memory_operand:
        tgt=ins.ip_rel_memory_address-IB
        if in_rdata(tgt):
            reg=ins.op_register(0)
            for nx in inss[i+1:i+5]:
                if nx.mnemonic==Mnemonic.MOV and nx.op_count==2 and nx.op_kind(0)==OpKind.MEMORY and nx.op_kind(1)==OpKind.REGISTER and nx.op_register(1)==reg and not nx.is_ip_rel_memory_operand:
                    installs.append((nx.ip-IB, tgt, nx.memory_displacement, rn(nx.memory_base)))
                    break
print("vtables instaladas en ctor 0x72D3B0:")
for rva,vt,disp,base in installs:
    print(f"  @ {hex(rva)}: vtable RVA {hex(vt)} -> [{base}+{hex(disp)}]")
def resolve_rtti(vtable_rva):
    col_va=u64(vtable_rva-8); col_rva=col_va-IB
    if not (in_rdata(col_rva) or in_data(col_rva)): return None
    td_rva=u32(col_rva+0xC)
    if not (in_data(td_rva) or in_rdata(td_rva)): return None
    return col_rva,td_rva,readstr(td_rva+0x10)
print("\nRTTI:")
for rva,vt,disp,base in installs:
    r=resolve_rtti(vt)
    tag=f"[{base}+{hex(disp)}]"
    if r:
        col,td,name=r
        print(f"  vtable {hex(vt)} {tag}: TD@{hex(td)} name={name}")
    else:
        print(f"  vtable {hex(vt)} {tag}: RTTI no resoluble")
