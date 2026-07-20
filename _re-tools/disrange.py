exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax
import sys
fmt=Formatter(FormatterSyntax.INTEL)
def readstr(rva,maxlen=80):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')
START=int(sys.argv[1],16); END=int(sys.argv[2],16)
for ins in Decoder(64,data[START:END],ip=IB+START):
    rva=ins.ip-IB; ex=""
    if ins.is_ip_rel_memory_operand:
        t=ins.ip_rel_memory_address-IB; ex=f"  ; ->{hex(t)}"
        if in_rdata(t):
            s=readstr(t)
            if s.isprintable() and 1<len(s)<60: ex+=f' "{s}"'
        elif in_data(t): ex+=" [.data]"
    print(f"  {hex(rva)}: {fmt.format(ins)}{ex}")
