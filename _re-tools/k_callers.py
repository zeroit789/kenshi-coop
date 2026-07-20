exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, OpKind, Mnemonic, Formatter, FormatterSyntax

fmt=Formatter(FormatterSyntax.INTEL)
def disasm_func(start,end,maxn=400):
    b=data[start:end]
    dec=Decoder(64,b,ip=IB+start)
    out=[]
    for ins in dec:
        out.append(ins)
    return out

# Para cada caller, buscar instalacion de vtable: "lea rax,[rip+vtbl]; mov [reg], rax"
# o "lea rcx,[rip+vtbl]". Las vtables instaladas dan la clase.
for cstart in [0x6e4f50,0x6e9c70]:
    fc=func_containing(cstart)
    cend=fc[1] if fc else cstart+0x400
    print(f"\n===== caller func {hex(cstart)} (end {hex(cend)}, size {cend-cstart}) =====")
    inss=disasm_func(cstart,cend)
    # mostrar primeras y buscar lea rip-rel a .rdata (posible vtable) y calls
    for ins in inss:
        rva=ins.ip-IB
        if ins.is_ip_rel_memory_operand:
            tgt=ins.ip_rel_memory_address-IB
            if in_rdata(tgt) and ins.mnemonic==Mnemonic.LEA:
                print(f"  {hex(rva)}: {fmt.format(ins)}   ; rdata target {hex(tgt)}")
        if ins.mnemonic==Mnemonic.CALL and ins.op_count==1 and ins.op_kind(0)==OpKind.NEAR_BRANCH64:
            t=ins.near_branch_target-IB
            # resolver thunk
            print(f"  {hex(rva)}: call {hex(t)}")
