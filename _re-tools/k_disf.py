exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind
fmt=Formatter(FormatterSyntax.INTEL)
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
print("=== Prologo 0x72D3B0 ===")
for rva,txt,ex in dis(0x72d3b0,0x72d3b0+60):
    print(f"  {hex(rva)}: {txt}{ex}")

print("\n=== Region creacion PausedPanel 0x72f7c0..0x72f8e0 ===")
for rva,txt,ex in dis(0x72f7c0,0x72f8e0):
    print(f"  {hex(rva)}: {txt}{ex}")
