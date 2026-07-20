exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
import iced_x86
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)
def readstr(rva,maxlen=120):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')
def disf(start):
    fc=func_containing(start); b,e=fc
    print(f"=== func {hex(b)}..{hex(e)} size {e-b} ===")
    dec=Decoder(64,data[b:e],ip=IB+b)
    for ins in dec:
        rva=ins.ip-IB; ex=""
        if ins.is_ip_rel_memory_operand:
            t=ins.ip_rel_memory_address-IB; ex=f"  ; ->{hex(t)}"
            if in_rdata(t):
                s=readstr(t)
                if s.isprintable() and 2<len(s)<60: ex+=f' "{s}"'
        print(f"  {hex(rva)}: {fmt.format(ins)}{ex}")
disf(0x6e9c70)
