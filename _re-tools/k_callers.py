# ES: Para los llamadores 0x6E4F50 y 0x6E9C70 del constructor investigado, desensambla su función y
#     muestra los lea RIP-relativos a .rdata (posibles vtables instaladas, que revelan la clase) y
#     los call directos. Carga k_setup.py desde C:/Users/Zero/ktmp. Uso: python k_callers.py
# EN: For callers 0x6E4F50 and 0x6E9C70 of the constructor under investigation, disassembles their
#     function and shows RIP-relative leas into .rdata (possible installed vtables, which reveal the
#     class) and direct calls. Loads k_setup.py from C:/Users/Zero/ktmp. Usage: python k_callers.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, OpKind, Mnemonic, Formatter, FormatterSyntax

fmt=Formatter(FormatterSyntax.INTEL)
# ES: Devuelve la lista de instrucciones de [start, end) (maxn no se usa).
# EN: Returns the instruction list for [start, end) (maxn is unused).
def disasm_func(start,end,maxn=400):
    b=data[start:end]
    dec=Decoder(64,b,ip=IB+start)
    out=[]
    for ins in dec:
        out.append(ins)
    return out

# Para cada caller, buscar instalacion de vtable: "lea rax,[rip+vtbl]; mov [reg], rax"
# o "lea rcx,[rip+vtbl]". Las vtables instaladas dan la clase.
# EN: For each caller, look for a vtable install: "lea rax,[rip+vtbl]; mov [reg], rax"
#     or "lea rcx,[rip+vtbl]". The installed vtables reveal the class.
for cstart in [0x6e4f50,0x6e9c70]:
    fc=func_containing(cstart)
    cend=fc[1] if fc else cstart+0x400
    print(f"\n===== caller func {hex(cstart)} (end {hex(cend)}, size {cend-cstart}) =====")
    inss=disasm_func(cstart,cend)
    # mostrar primeras y buscar lea rip-rel a .rdata (posible vtable) y calls
    # EN: show the first ones and look for rip-rel lea into .rdata (possible vtable) and calls
    for ins in inss:
        rva=ins.ip-IB
        if ins.is_ip_rel_memory_operand:
            tgt=ins.ip_rel_memory_address-IB
            if in_rdata(tgt) and ins.mnemonic==Mnemonic.LEA:
                print(f"  {hex(rva)}: {fmt.format(ins)}   ; rdata target {hex(tgt)}")
        if ins.mnemonic==Mnemonic.CALL and ins.op_count==1 and ins.op_kind(0)==OpKind.NEAR_BRANCH64:
            t=ins.near_branch_target-IB
            # resolver thunk
            # EN: resolve thunk (not implemented: the raw target is printed)
            print(f"  {hex(rva)}: call {hex(t)}")
