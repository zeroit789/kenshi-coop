# writes 'mov [reg+0xE8],regPtr' cuya funcion referencia vtable Tasker 0x16BDC68 o llama al ctor Tasker.
import pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax, OpKind, Register
EXE=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(EXE,fast_load=True); data=open(EXE,"rb").read()
for s in pe.sections:
    if b".text" in s.Name: text=s;break
base=text.VirtualAddress; raw=text.PointerToRawData; size=text.SizeOfRawData
b=data[raw:raw+size]
dec=Decoder(64,b,ip=IB+base); fmt=Formatter(FormatterSyntax.INTEL)
writes=[]
for ins in dec:
    if (ins.op0_kind==OpKind.MEMORY and ins.memory_displacement==0xE8 and ins.memory_base not in (Register.NONE,Register.RSP,Register.RBP,Register.RIP)
        and ins.op1_kind==OpKind.REGISTER and ins.op1_register not in (Register.EAX,Register.EBP)):
        writes.append((ins.ip-IB, fmt.format(ins)))
VTS={0x16BDC68,0x16BDD88,0x16BE448,0x16BE1D8}  # Tasker, Task_Move, Melee, FollowLeader
def ctx(rva):
    lo=max(base,rva-0x600); hi=rva+0x80
    d=Decoder(64,data[raw+(lo-base):raw+(hi-base)],ip=IB+lo)
    found=set()
    for ins in d:
        if ins.is_ip_rel_memory_operand:
            t=ins.ip_rel_memory_address-IB
            if t in VTS: found.add(t)
    return found
for rva,m in writes:
    f=ctx(rva)
    if f:
        names={0x16BDC68:"Tasker",0x16BDD88:"Move",0x16BE448:"Melee",0x16BE1D8:"FollowLeader"}
        print("0x%X: %-26s  vtrefs: %s" % (rva,m,[names[x] for x in f]))
print("--- done ---")
