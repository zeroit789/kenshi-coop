# ES: Lista todas las ramas directas (call/jmp) en .text hacia el RVA indicado y la función (.pdata)
#     que contiene cada una. Carga k_setup.py desde C:/Users/Zero/ktmp.
#     Uso: python k_callers2.py <rva_hex>
# EN: Lists every direct branch (call/jmp) in .text to the given RVA and the function (.pdata) that
#     contains each one. Loads k_setup.py from C:/Users/Zero/ktmp.
#     Usage: python k_callers2.py <rva_hex>

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, OpKind, Mnemonic
import sys
TARGET=int(sys.argv[1],16)
target_va=IB+TARGET
text=data[TEXT_RVA:TEXT_RVA+TEXT_SZ]
dec=Decoder(64,text,ip=IB+TEXT_RVA)
xr=[]
for ins in dec:
    if ins.mnemonic in (Mnemonic.CALL,Mnemonic.JMP) and ins.op_count and ins.op_kind(0)==OpKind.NEAR_BRANCH64:
        if ins.near_branch_target==target_va:
            fc=func_containing(ins.ip-IB)
            xr.append((ins.ip-IB,str(ins.mnemonic),hex(fc[0]) if fc else '?'))
    # tambien call indirecto via ip-rel (call [rip+x]) apuntando a target? raro
# EN: also an indirect call through ip-rel (call [rip+x]) pointing to target? rare (not implemented)
print(f"branch-xrefs a {hex(TARGET)}: {len(xr)}")
for rva,mn,fn in xr: print(f"  RVA {hex(rva)} {mn:6} en func {fn}")
