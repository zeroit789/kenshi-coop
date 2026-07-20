import pefile
from iced_x86 import Decoder, Mnemonic, OpKind
PATH=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(PATH, fast_load=True)
txt=[s for s in pe.sections if s.Name.rstrip(b'\x00')==b'.text'][0]
tva=txt.VirtualAddress
def deref_jmp(rva):
    data=pe.get_data(rva,8); d=Decoder(64,data,ip=IB+rva)
    ins=next(iter(d))
    if ins.mnemonic==Mnemonic.JMP and ins.op0_kind==OpKind.NEAR_BRANCH64:
        return ins.near_branch_target-IB
    return rva
# init thunks
for name,t in [("init_vt1",0x44445),("init_vt2",0x1D0C0),("gb_vt1",0x50C45),("gb_vt2",0x3B269),("gb_vt3",0xA687),("reg",0x477BC)]:
    print(f"{name}: thunk 0x{t:X} -> 0x{deref_jmp(t):X}")
