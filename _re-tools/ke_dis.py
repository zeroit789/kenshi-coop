# Desensamblado extendido READ-ONLY. Uso: python ke_dis.py <rva_hex> <nbytes> [count]
import sys, pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax
EXE = r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
IB = 0x140000000
pe = pefile.PE(EXE, fast_load=True)
data = open(EXE,"rb").read()
def dis(rva, n, count):
    off = pe.get_offset_from_rva(rva)
    b = data[off:off+n]
    dec = Decoder(64, b, ip=IB+rva)
    fmt = Formatter(FormatterSyntax.INTEL)
    i=0
    for ins in dec:
        if i>=count: break
        o = ins.ip-(IB+rva)
        raw=" ".join(f"{x:02X}" for x in b[o:o+ins.len])
        print(f"0x{ins.ip-IB:X}: {fmt.format(ins):<46} ; {raw}")
        i+=1
rva=int(sys.argv[1],16)
n=int(sys.argv[2]) if len(sys.argv)>2 else 160
c=int(sys.argv[3]) if len(sys.argv)>3 else 50
dis(rva,n,c)
