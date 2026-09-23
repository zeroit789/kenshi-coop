# ES: Desensambla el rango [START, END) de kenshi_x64.exe y anota cada operando RIP-relativo con su
#     destino; si cae en .rdata y parece cadena la imprime, si cae en .data lo marca.
#     Depende de k_setup.py y k_regs.py cargados con exec() desde C:/Users/Zero/ktmp (ruta antigua;
#     las copias versionadas están en _re-tools/). Uso: python disrange.py <start_hex> <end_hex>
# EN: Disassembles range [START, END) of kenshi_x64.exe and annotates every RIP-relative operand with
#     its target; if it lands in .rdata and looks like a string it prints it, if in .data it tags it.
#     Depends on k_setup.py and k_regs.py loaded via exec() from C:/Users/Zero/ktmp (old path;
#     the versioned copies live in _re-tools/). Usage: python disrange.py <start_hex> <end_hex>

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax
import sys
fmt=Formatter(FormatterSyntax.INTEL)
# ES: Lee una cadena terminada en 0 (latin1) desde un RVA.
# EN: Reads a NUL-terminated string (latin1) from an RVA.
def readstr(rva,maxlen=80):
    b=bytearray(); i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen: b.append(data[i]); i+=1
    return b.decode('latin1','replace')
# ES: Recorre el rango pedido y anota destinos RIP-relativos.
# EN: Walks the requested range and annotates RIP-relative targets.
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
