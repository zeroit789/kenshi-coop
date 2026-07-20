exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Mnemonic, OpKind

# El container del HUD .data = 0x21337B0. Veamos quien le pasa el init.
# En 0x36ccc5 se llama init(0x21337B0). Confirmar que 0x21337B0 es donde el init guarda MainBarGUI+0x10.
# El init recibe rcx=container; guarda MainBarGUI en [container+0x10].
# Buscar TODOS los callers de 0x6E20D0 (handler pausa) y del thunk 0x266E8->0x6E20D0
T1=IB+0x6E20D0
T2=IB+0x266E8
calls={T1:[],T2:[]}
dec=Decoder(64,data[TEXT_RVA:TEXT_RVA+TEXT_SZ],ip=IB+TEXT_RVA)
for ins in dec:
    if ins.mnemonic==Mnemonic.CALL and ins.op0_kind==OpKind.NEAR_BRANCH64:
        t=ins.near_branch_target
        if t in calls:
            rva=ins.ip-IB; fc=func_containing(rva)
            calls[t].append((rva,fc))
print("Callers de 0x6E20D0 (handler directo):")
for rva,fc in calls[T1]:
    print(f"  {hex(rva)} en {hex(fc[0]) if fc else '??'}")
print("Callers de thunk 0x266E8 -> 0x6E20D0:")
for rva,fc in calls[T2]:
    print(f"  {hex(rva)} en {hex(fc[0]) if fc else '??'}")

# Confirmar la cadena de offsets en 0x6E20D0
print("\n0x720F50 devuelve [rcx+0x2C8] = PausedPanel. CONFIRMADO offset coincide con ctor (lea rdx,[rdi+2C8h] @0x72f7f4).")
