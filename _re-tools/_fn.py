# ES: Estima el inicio de la función que contiene el RVA 0x623AFB en kenshi_x64.exe: retrocede
#     por .text hasta encontrar el relleno int3 (CC CC CC) que suele preceder a una función.
#     Uso: python _fn.py. Imprime "func start ~0x...".
# EN: Estimates the start of the function containing RVA 0x623AFB in kenshi_x64.exe: walks back
#     through .text until it finds the int3 padding (CC CC CC) that usually precedes a function.
#     Usage: python _fn.py. Prints "func start ~0x...".

import pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic
PATH=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(PATH, fast_load=True)
sec=[s for s in pe.sections if s.Name.rstrip(b'\x00')==b'.text'][0]
va=sec.VirtualAddress; data=pe.get_data(va, sec.Misc_VirtualSize)
# escanear hacia atras desde 0x623AFB buscando prologo tipico (int3 padding then push/sub/mov rax,rsp)
# EN: scan backwards from 0x623AFB looking for a typical prologue (int3 padding then push/sub/mov rax,rsp)
target=0x623AFB
# buscar ultimo 'cc cc cc' antes del target seguido por inicio de func
# EN: find the last 'cc cc cc' before the target followed by a function start
start=None
for off in range(target-va, max(0,target-va-0x2000), -1):
    if data[off-3:off]==b'\xcc\xcc\xcc' and data[off]!=0xcc:
        start=va+off; break
print("func start ~0x%X"%start)
