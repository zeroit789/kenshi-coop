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
print(f"branch-xrefs a {hex(TARGET)}: {len(xr)}")
for rva,mn,fn in xr: print(f"  RVA {hex(rva)} {mn:6} en func {fn}")
