import ke_re as k, struct
pe,data=k._load()
base_rva=0x622200; n=0x900
b=k.bytes_at_rva(base_rva,n)
from iced_x86 import Decoder, Formatter, FormatterSyntax
dec=Decoder(64,b,ip=0x140000000+base_rva)
fmt=Formatter(FormatterSyntax.INTEL)
for ins in dec:
    s=fmt.format(ins)
    # interesa: mov [reg+0x648], / [reg+0x448], / call (factories) / lea vtable
    if ('+648h]' in s and 'mov' in s) or ('+448h]' in s and 'mov' in s) or ('call ' in s):
        rva=ins.ip-0x140000000
        raw=" ".join(f"{x:02X}" for x in b[rva-base_rva:rva-base_rva+ins.len])
        print(f"0x{rva:X}: {s:<44} ; {raw}")
