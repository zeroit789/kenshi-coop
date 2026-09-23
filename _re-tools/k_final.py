# ES: Comprobación final del panel de pausa: lee la constante float en .rdata 0x1681B38, desensambla
#     la cola de setPaused (desde 0x787DCC) marcando destinos de call y decodifica el thunk 0x266E8.
#     Carga k_setup.py/k_regs.py desde C:/Users/Zero/ktmp. Uso: python k_final.py
# EN: Final pause-panel check: reads the float constant at .rdata 0x1681B38, disassembles the tail of
#     setPaused (from 0x787DCC) marking call targets and decodes thunk 0x266E8.
#     Loads k_setup.py/k_regs.py from C:/Users/Zero/ktmp. Usage: python k_final.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind
fmt=Formatter(FormatterSyntax.INTEL)
import struct as _s
# Constante en 0x1681b38
# EN: Constant at 0x1681b38
val=_s.unpack_from("<f",data,0x1681b38)[0]
print("const float @0x1681b38 =", val)
# Desensamblar la cola de setPaused desde 0x787dd0
# EN: Disassemble the tail of setPaused from 0x787dd0 (starts at 0x787dcc)
b=0x787dcc
dec=Decoder(64,data[b:b+40],ip=IB+b)
for ins in dec:
    rva=ins.ip-IB; ex=""
    if ins.is_ip_rel_memory_operand:
        ex=f"  ; ->{hex(ins.ip_rel_memory_address-IB)}"
    txt=fmt.format(ins)
    # resolver thunk en calls
    # EN: resolve thunks in calls
    if ins.mnemonic==Mnemonic.CALL and ins.op_count==1 and ins.op_kind(0)==OpKind.NEAR_BRANCH64:
        t=ins.near_branch_target-IB
        # leer si es thunk jmp
        # EN: check whether it is a jmp thunk (no-op: only the target is printed)
        if data[t]==0xE9 or (data[t]==0xFF):
            pass
        ex+=f"  [call->{hex(t)}]"
    print(f"  {hex(rva)}: {txt}{ex}")
# Resolver thunk 0x266e8
# EN: Resolve thunk 0x266e8
print("\nthunk 0x266e8:", ' '.join(f'{data[0x266e8+i]:02X}' for i in range(5)))
d2=Decoder(64,data[0x266e8:0x266e8+5],ip=IB+0x266e8)
for ins in d2:
    print("  ->", fmt.format(ins), "=>", hex(ins.near_branch_target-IB) if ins.op_count else "")
