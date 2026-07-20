exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, OpKind, Mnemonic, Formatter, FormatterSyntax

def disasm_range(rva_start, count=8):
    b = data[rva_start:rva_start+count*16]
    dec = Decoder(64, b, ip=IB+rva_start)
    fmt = Formatter(FormatterSyntax.INTEL)
    out=[]
    for ins in dec:
        if len(out)>=count: break
        out.append((ins.ip-IB, fmt.format(ins), ins.len))
    return out

print("=== Contexto en 0x129a4 (el xref) ===")
for rva,txt,ln in disasm_range(0x12990, 10):
    print(f"  {hex(rva)}: {txt}")

# Es probable que 0x129a4 sea un thunk JMP. Veamos a que VA salta y si hay
# xrefs a la direccion del propio thunk (inicio del thunk).
# El thunk normalmente empieza justo en 0x129a4 o un poco antes.
