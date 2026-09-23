# ES: Desensambla 10 instrucciones alrededor de 0x129A4 (el xref encontrado al constructor 0x72D3B0)
#     para ver si es un thunk JMP. Carga k_setup.py desde C:/Users/Zero/ktmp. Uso: python k_thunk.py
# EN: Disassembles 10 instructions around 0x129A4 (the xref found to constructor 0x72D3B0) to see
#     whether it is a JMP thunk. Loads k_setup.py from C:/Users/Zero/ktmp. Usage: python k_thunk.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, OpKind, Mnemonic, Formatter, FormatterSyntax

# ES: Devuelve (rva, texto, longitud) de las count primeras instrucciones desde rva_start.
# EN: Returns (rva, text, length) for the first count instructions from rva_start.
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
# EN: 0x129a4 is probably a JMP thunk. Let us see which VA it jumps to and whether there are
#     xrefs to the thunk address itself (thunk start).
#     The thunk usually starts right at 0x129a4 or slightly before.
#     (Notes only: the script ends here.)
