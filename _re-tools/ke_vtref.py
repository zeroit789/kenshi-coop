# Busca lea reg,[vtable_rva] (referencia RIP-rel a la vtable) en .text. READ-ONLY.
import sys,pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax, OpKind
EXE=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(EXE,fast_load=True); data=open(EXE,"rb").read()
for s in pe.sections:
    if b".text" in s.Name: text=s;break
base=text.VirtualAddress; raw=text.PointerToRawData; size=text.SizeOfRawData
dec=Decoder(64,data[raw:raw+size],ip=IB+base); fmt=Formatter(FormatterSyntax.INTEL)
vt=int(sys.argv[1],16)
for ins in dec:
    if ins.is_ip_rel_memory_operand and (ins.ip_rel_memory_address-IB)==vt:
        print("0x%X: %s" % (ins.ip-IB, fmt.format(ins)))
print("--- refs a vtable 0x%X ---"%vt)
