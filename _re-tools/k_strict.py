exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Mnemonic, OpKind, Register

VT_MAIN=0x17099f8  # SOLO la especifica de MainBarGUI
tgt=IB+VT_MAIN
text_bytes=data[TEXT_RVA:TEXT_RVA+TEXT_SZ]
dec=Decoder(64,text_bytes,ip=IB+TEXT_RVA)
refs={}
for ins in dec:
    if ins.is_ip_rel_memory_operand and ins.ip_rel_memory_address==tgt:
        fc=func_containing(ins.ip-IB)
        f=fc[0] if fc else None
        refs.setdefault(f,[]).append(ins.ip-IB)
print(f"funcs que referencian EXCLUSIVAMENTE vtable MainBarGUI 0x{VT_MAIN:x}:")
for f,locs in sorted(refs.items(), key=lambda x:(x[0] or 0)):
    print(f"  func {hex(f) if f else '?'}: @ {[hex(x) for x in locs]}")
