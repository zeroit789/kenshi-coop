# ES: Decodifica exactamente una instrucción en cada RVA pasado e imprime bytes y texto Intel.
#     Depende de ke_dis.load()/ke_dis._image, que el ke_dis.py versionado no define.
#     Uso: python ke_one.py <rva_hex> [...]
# EN: Decodes exactly one instruction at each given RVA and prints bytes and Intel text.
#     Depends on ke_dis.load()/ke_dis._image, which the versioned ke_dis.py does not define.
#     Usage: python ke_one.py <rva_hex> [...]

# decodifica exactamente desde un RVA dado, 1 instruccion
# EN: decodes exactly 1 instruction starting at a given RVA
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
