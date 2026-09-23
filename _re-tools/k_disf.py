# ES: Muestra el prólogo de la función 0x72D3B0 (60 bytes) y la región 0x72F7C0-0x72F8E0 donde se
#     crea el PausedPanel (panel de pausa), anotando destinos RIP-relativos. Carga k_setup.py desde
#     C:/Users/Zero/ktmp. Uso: python k_disf.py
# EN: Shows the prologue of function 0x72D3B0 (60 bytes) and the 0x72F7C0-0x72F8E0 region where the
#     PausedPanel (pause panel) is created, annotating RIP-relative targets. Loads k_setup.py from
#     C:/Users/Zero/ktmp. Usage: python k_disf.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind
fmt=Formatter(FormatterSyntax.INTEL)
# ES: Generador: (rva, texto Intel, anotación del destino RIP-relativo) para [start, end).
# EN: Generator: (rva, Intel text, RIP-relative target note) for [start, end).
def dis(start,end):
    b=data[start:end]
    dec=Decoder(64,b,ip=IB+start)
    for ins in dec:
        rva=ins.ip-IB
        extra=""
        if ins.is_ip_rel_memory_operand:
            t=ins.ip_rel_memory_address-IB
            extra=f"  ; ->{hex(t)}"
        yield rva, fmt.format(ins), extra

# Prologo de 0x72D3B0
# EN: Prologue of 0x72D3B0
print("=== Prologo 0x72D3B0 ===")
for rva,txt,ex in dis(0x72d3b0,0x72d3b0+60):
    print(f"  {hex(rva)}: {txt}{ex}")

print("\n=== Region creacion PausedPanel 0x72f7c0..0x72f8e0 ===")
for rva,txt,ex in dis(0x72f7c0,0x72f8e0):
    print(f"  {hex(rva)}: {txt}{ex}")
