import pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic
PATH=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(PATH, fast_load=True)
sec=[s for s in pe.sections if s.Name.rstrip(b'\x00')==b'.text'][0]
va=sec.VirtualAddress; data=pe.get_data(va, sec.Misc_VirtualSize)
# escanear hacia atras desde 0x623AFB buscando prologo tipico (int3 padding then push/sub/mov rax,rsp)
target=0x623AFB
# buscar ultimo 'cc cc cc' antes del target seguido por inicio de func
start=None
for off in range(target-va, max(0,target-va-0x2000), -1):
    if data[off-3:off]==b'\xcc\xcc\xcc' and data[off]!=0xcc:
        start=va+off; break
print("func start ~0x%X"%start)
