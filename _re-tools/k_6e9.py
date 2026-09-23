# ES: Desensambla entera la función que contiene 0x6E9C70 (uno de los llamadores del constructor
#     0x72D3B0) anotando destinos RIP-relativos y cadenas de .rdata. Carga k_setup.py desde la ruta
#     antigua C:/Users/Zero/ktmp. Uso: python k_6e9.py
# EN: Disassembles the whole function containing 0x6E9C70 (one of the callers of constructor
#     0x72D3B0) annotating RIP-relative targets and .rdata strings. Loads k_setup.py from the old
#     path C:/Users/Zero/ktmp. Usage: python k_6e9.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
import iced_x86
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)
# ES: Lee una cadena terminada en 0 (latin1) desde un RVA.
# EN: Reads a NUL-terminated string (latin1) from an RVA.
def readstr(rva,maxlen=120):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')
# ES: Imprime el desensamblado de la función (.pdata) que contiene start.
# EN: Prints the disassembly of the function (.pdata) containing start.
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
