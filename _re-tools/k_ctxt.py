exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, OpKind, Mnemonic, Register
fmt=Formatter(FormatterSyntax.INTEL)
def readstr(rva,maxlen=80):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')
import sys
TARGET=int(sys.argv[1],16)
# Si no esta en pdata, desensamblar un rango fijo
fc=func_containing(TARGET)
if fc: b,e=fc
else:
    b=TARGET; e=TARGET+int(sys.argv[2],16) if len(sys.argv)>2 else TARGET+0x120
print(f"=== {hex(b)}..{hex(e)} size {e-b} (pdata={'Y' if fc else 'N'}) ===")
for ins in Decoder(64,data[b:e],ip=IB+b):
    rva=ins.ip-IB; ex=""
    if ins.is_ip_rel_memory_operand:
        t=ins.ip_rel_memory_address-IB; ex=f"  ; ->{hex(t)}"
        if in_rdata(t):
            s=readstr(t)
            if s.isprintable() and 2<len(s)<60: ex+=f' "{s}"'
        elif in_data(t): ex+=" [.data]"
    if ins.mnemonic in (Mnemonic.CALL,Mnemonic.JMP) and ins.op_count and ins.op_kind(0)==OpKind.NEAR_BRANCH64:
        ex+=f"  ; call->{hex(ins.near_branch_target-IB)}"
    print(f"  {hex(rva)}: {fmt.format(ins)}{ex}")
