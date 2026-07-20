# decodifica exactamente desde un RVA dado, 1 instruccion
import sys, ke_dis
from iced_x86 import Decoder, Formatter, FormatterSyntax
ke_dis.load(); img=ke_dis._image; IB=0x140000000
for a in sys.argv[1:]:
    rva=int(a,16)
    code=bytes(img[rva:rva+16])
    dec=Decoder(64,code,ip=IB+rva)
    fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_suffix="h"
    ins=next(iter(dec))
    raw=" ".join(f"{b:02X}" for b in code[:ins.len])
    print(f"0x{rva:08X}  {raw:<24}  {fmt.format(ins)}")
