# ES: Desensamblador de línea de comandos (solo lectura): imprime count instrucciones desde el RVA,
#     leyendo nbytes del fichero, con los bytes crudos de cada una.
#     Uso: python ke_dis.py <rva_hex> [nbytes=160] [count=50]
#     Nota: ke_call.py y ke_find.py esperan ke_dis.load() y ke_dis._image, que este fichero no define.
# EN: Command-line disassembler (read-only): prints count instructions from the RVA, reading nbytes
#     from the file, with each instruction's raw bytes.
#     Usage: python ke_dis.py <rva_hex> [nbytes=160] [count=50]
#     Note: ke_call.py and ke_find.py expect ke_dis.load() and ke_dis._image, which this file does not define.

# Desensamblado extendido READ-ONLY. Uso: python ke_dis.py <rva_hex> <nbytes> [count]
# EN: Extended disassembly, READ-ONLY. Usage: python ke_dis.py <rva_hex> <nbytes> [count]
import sys, pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax
EXE = r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
IB = 0x140000000
pe = pefile.PE(EXE, fast_load=True)
data = open(EXE,"rb").read()
# ES: Desensambla n bytes desde rva e imprime hasta count instrucciones.
# EN: Disassembles n bytes from rva and prints up to count instructions.
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
# ES: Argumentos de línea de comandos.
# EN: Command-line arguments.
rva=int(sys.argv[1],16)
n=int(sys.argv[2]) if len(sys.argv)>2 else 160
c=int(sys.argv[3]) if len(sys.argv)>3 else 50
dis(rva,n,c)
