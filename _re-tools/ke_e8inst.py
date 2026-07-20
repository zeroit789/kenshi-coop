# Busca el patron INSTALADOR: dentro de una ventana corta, mov regA,[regB+0x448] seguido de mov [regA+0xE8], regPtr
# READ-ONLY. Decodifica .text linealmente y mantiene ventana de instrucciones.
import pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax, OpKind, Register
EXE=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(EXE,fast_load=True); data=open(EXE,"rb").read()
for s in pe.sections:
    if b".text" in s.Name: text=s;break
b=data[text.PointerToRawData:text.PointerToRawData+text.SizeOfRawData]
dec=Decoder(64,b,ip=IB+text.VirtualAddress); fmt=Formatter(FormatterSyntax.INTEL)
# track: para cada registro, en que IP fue cargado con [X+0x448]
last448={}   # reg -> ip
for ins in dec:
    rip=ins.ip-IB
    # detecta mov regA, [regB+0x448]
    if ins.mnemonic_name if False else True:
        pass
    m=fmt.format(ins)
    # caso load +0x448
    if ins.op0_kind==OpKind.REGISTER and ins.op1_kind==OpKind.MEMORY and ins.memory_displacement==0x448:
        last448[ins.op0_register]=rip
    # caso write [regA+0xE8], regPtr  donde regA fue cargado de +0x448 recientemente
    if ins.op0_kind==OpKind.MEMORY and ins.memory_displacement==0xE8 and ins.op1_kind==OpKind.REGISTER:
        base=ins.memory_base
        if base in last448 and (rip-last448[base])<0x120:
            print(f"0x{rip:X}: {m}   <-- base cargado de +0x448 en 0x{last448[base]:X} (delta {rip-last448[base]:X})")
print("--- fin scan ---")
