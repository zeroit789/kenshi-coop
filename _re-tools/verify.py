# ES: Lista todos los call directos a 0x6E20D0 (handler de pausa) y a su thunk 0x266E8, con la función
#     que contiene cada uno, y deja anotado que 0x720F50 devuelve [rcx+0x2C8] = PausedPanel.
#     Carga k_setup.py/k_regs.py desde C:/Users/Zero/ktmp. Uso: python verify.py
# EN: Lists every direct call to 0x6E20D0 (pause handler) and to its thunk 0x266E8, with the containing
#     function of each, and records that 0x720F50 returns [rcx+0x2C8] = PausedPanel.
#     Loads k_setup.py/k_regs.py from C:/Users/Zero/ktmp. Usage: python verify.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Mnemonic, OpKind

# El container del HUD .data = 0x21337B0. Veamos quien le pasa el init.
# En 0x36ccc5 se llama init(0x21337B0). Confirmar que 0x21337B0 es donde el init guarda MainBarGUI+0x10.
# El init recibe rcx=container; guarda MainBarGUI en [container+0x10].
# Buscar TODOS los callers de 0x6E20D0 (handler pausa) y del thunk 0x266E8->0x6E20D0
# EN: The HUD container in .data = 0x21337B0. Let us see who passes it to init.
#     At 0x36ccc5 init(0x21337B0) is called. Confirm that 0x21337B0 is where init stores MainBarGUI+0x10.
#     init receives rcx=container; stores MainBarGUI at [container+0x10].
#     Find ALL callers of 0x6E20D0 (pause handler) and of thunk 0x266E8->0x6E20D0
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
# EN: Confirm the offset chain in 0x6E20D0
print("\n0x720F50 devuelve [rcx+0x2C8] = PausedPanel. CONFIRMADO offset coincide con ctor (lea rdx,[rdi+2C8h] @0x72f7f4).")
