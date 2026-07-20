exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Mnemonic, OpKind, Register

VT=0x17099f8  # MainBarGUI vtable principal

# Resolver thunk map (igual que antes) para des-thunkear destinos
def build_thunkmap():
    tb=data[0x12000:0x14000]
    dec=Decoder(64,tb,ip=IB+0x12000); tm={}
    for ins in dec:
        if ins.mnemonic==Mnemonic.JMP and ins.op_count==1 and ins.op_kind(0)==OpKind.NEAR_BRANCH64 and ins.len==5:
            tm[ins.ip-IB]=ins.near_branch_target-IB
    return tm
TM=build_thunkmap()
def resolve(rva):
    return TM.get(rva,rva)

# Leer vtable: secuencia de qwords (VAs) hasta que deje de apuntar a .text/thunk
methods=[]
i=0
while True:
    q=u64(VT+i*8)
    if q==0: break
    r=q-IB
    if not in_text(r): break
    methods.append((i, r, resolve(r)))
    i+=1
    if i>80: break
print(f"vtable MainBarGUI 0x{VT:x}: {len(methods)} slots")
for idx,raw,real in methods:
    fc=func_containing(real)
    print(f"  slot[{idx:2}] off+{hex(idx*8):>6}: {hex(real)}"+(f" (thunk {hex(raw)})" if raw!=real else ""))
