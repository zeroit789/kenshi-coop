# ES: Desensambla entera la función (según .pdata) que contiene 0x494AF0 y anota los destinos
#     RIP-relativos, mostrando la cadena si apuntan a .rdata. Carga k_setup.py desde la ruta antigua
#     C:/Users/Zero/ktmp. Uso: python k_494.py
# EN: Disassembles the whole function (per .pdata) containing 0x494AF0 and annotates RIP-relative
#     targets, showing the string when they point into .rdata. Loads k_setup.py from the old path
#     C:/Users/Zero/ktmp. Usage: python k_494.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
import iced_x86
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)
# ES: Lee una cadena terminada en 0 (latin1) desde un RVA.
# EN: Reads a NUL-terminated string (latin1) from an RVA.
def readstr(rva,maxlen=120):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen:
        b.append(data[i]); i+=1
    return b.decode('latin1','replace')

# ES: Localiza la función y la recorre instrucción a instrucción.
# EN: Locate the function and walk it instruction by instruction.
fc=func_containing(0x494af0)
print("func 0x494af0 rango:",hex(fc[0]),hex(fc[1]),"size",fc[1]-fc[0])
b,e=fc
dec=Decoder(64,data[b:e],ip=IB+b)
for ins in dec:
    rva=ins.ip-IB
    ex=""
    if ins.is_ip_rel_memory_operand:
        t=ins.ip_rel_memory_address-IB
        ex=f"  ; ->{hex(t)}"
        if in_rdata(t):
            s=readstr(t)
            if s.isprintable() and len(s)>2: ex+=f' "{s}"'
    print(f"  {hex(rva)}: {fmt.format(ins)}{ex}")
