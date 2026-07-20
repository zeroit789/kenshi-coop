exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, OpKind, Mnemonic, Formatter, FormatterSyntax
fmt=Formatter(FormatterSyntax.INTEL)

# Strings de interes
STR = {
 0x170AA48:'PausedPanel',
 0x170AA38:'lbPaused',
 0x170AA30:'PAUSED',
}
targets={IB+r:n for r,n in STR.items()}

# Barrido lineal de .text; recoger toda ins ip-rel que apunte a esos strings
text_bytes=data[TEXT_RVA:TEXT_RVA+TEXT_SZ]
dec=Decoder(64,text_bytes,ip=IB+TEXT_RVA)
hits=[]
for ins in dec:
    if ins.is_ip_rel_memory_operand:
        a=ins.ip_rel_memory_address
        if a in targets:
            hits.append((ins.ip-IB, targets[a], fmt.format(ins)))
print("xrefs a strings de pausa:")
seen=set()
for rva,nm,txt in hits:
    fc=func_containing(rva)
    f=hex(fc[0]) if fc else '?'
    print(f"  {hex(rva)} [{nm:11}] func={f:10} {txt}")
