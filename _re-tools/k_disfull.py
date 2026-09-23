# ES: Desensambla completa la función (.pdata) que contiene el RVA dado, anotando destinos
#     RIP-relativos (cadenas de .rdata y marcas de .data). Carga k_setup.py/k_regs.py con exec()
#     desde la ruta antigua C:/Users/Zero/ktmp. Uso: python k_disfull.py <rva_hex>
# EN: Disassembles the whole function (.pdata) containing the given RVA, annotating RIP-relative
#     targets (.rdata strings and .data tags). Loads k_setup.py/k_regs.py via exec() from the old
#     path C:/Users/Zero/ktmp. Usage: python k_disfull.py <rva_hex>

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)
# ES: Lee una cadena terminada en 0 (latin1) desde un RVA.
# EN: Reads a NUL-terminated string (latin1) from an RVA.
def readstr(rva,maxlen=80):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')
import sys
TARGET=int(sys.argv[1],16)
b,e=func_containing(TARGET)
print(f"=== func {hex(b)}..{hex(e)} size {e-b} ===")
for ins in Decoder(64,data[b:e],ip=IB+b):
    rva=ins.ip-IB; ex=""
    if ins.is_ip_rel_memory_operand:
        t=ins.ip_rel_memory_address-IB; ex=f"  ; ->{hex(t)}"
        if in_rdata(t):
            s=readstr(t)
            if s.isprintable() and 2<len(s)<60: ex+=f' "{s}"'
        elif in_data(t): ex+=" [.data]"
    print(f"  {hex(rva)}: {fmt.format(ins)}{ex}")
