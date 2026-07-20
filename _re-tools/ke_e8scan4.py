# Busca 'call ...; (pocas instr); mov [reg+0xE8], rax'  => instalacion de un objeto-fabricado en +0xE8.
# Reporta tambien si la func referencia vtable AnimationClass o el offset 0x448 / 0x658 (platoon) cerca.
import pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax, OpKind, Register, Mnemonic
EXE=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(EXE,fast_load=True); data=open(EXE,"rb").read()
for s in pe.sections:
    if b".text" in s.Name: text=s;break
base=text.VirtualAddress; raw=text.PointerToRawData; size=text.SizeOfRawData
b=data[raw:raw+size]
dec=Decoder(64,b,ip=IB+base); fmt=Formatter(FormatterSyntax.INTEL)
window=[]  # ultimas N instr (ip,mnem,isCall)
N=6
res=[]
for ins in dec:
    isw = (ins.op0_kind==OpKind.MEMORY and ins.memory_displacement==0xE8 and ins.op1_kind==OpKind.REGISTER
           and ins.op1_register==Register.RAX and ins.memory_base not in (Register.NONE,Register.RSP,Register.RIP))
    if isw:
        recent_call=any(w[2] for w in window[-N:])
        if recent_call:
            res.append((ins.ip-IB, fmt.format(ins)))
    window.append((ins.ip-IB, ins.mnemonic, ins.mnemonic==Mnemonic.CALL))
    if len(window)>20: window.pop(0)
print("writes [reg+0xE8],rax precedidas por call:")
for rva,m in res:
    print("0x%X: %s" % (rva,m))
print("--- %d hits ---" % len(res))
