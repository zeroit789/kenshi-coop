exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, OpKind, Mnemonic
TARGET = 0x72D3B0
target_va = IB + TARGET
text_bytes = data[TEXT_RVA:TEXT_RVA+TEXT_SZ]
dec = Decoder(64, text_bytes, ip=IB+TEXT_RVA)
xrefs=[]
for ins in dec:
    if ins.mnemonic in (Mnemonic.CALL, Mnemonic.JMP):
        for opi in range(ins.op_count):
            if ins.op_kind(opi) == OpKind.NEAR_BRANCH64:
                if ins.near_branch_target == target_va:
                    xrefs.append((ins.ip-IB, str(ins.mnemonic), 'branch'))
    if ins.is_ip_rel_memory_operand and ins.ip_rel_memory_address == target_va:
        xrefs.append((ins.ip-IB, str(ins.mnemonic), 'iprel'))
print("xrefs a 0x72D3B0:")
for rva,mn,kind in xrefs:
    fc = func_containing(rva)
    print(f"  RVA {hex(rva)} {mn:14} {kind} func={hex(fc[0]) if fc else '?'}")
