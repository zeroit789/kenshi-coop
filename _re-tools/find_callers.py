# ES: Verifica que 0x129A4 y 0x29C21 son thunks (jmp) y lista todas las llamadas directas a ellos en
#     .text, indicando la función (según .pdata) que contiene cada call. Targets: thunk del
#     constructor 0x72D3B0 y del init 0x6E4F50. Depende de k_setup.py/k_regs.py (exec desde la
#     ruta antigua C:/Users/Zero/ktmp). Uso: python find_callers.py
# EN: Verifies that 0x129A4 and 0x29C21 are thunks (jmp) and lists every direct call to them in
#     .text, showing the function (per .pdata) that contains each call. Targets: thunk of the
#     constructor 0x72D3B0 and of init 0x6E4F50. Depends on k_setup.py/k_regs.py (exec from the
#     old path C:/Users/Zero/ktmp). Usage: python find_callers.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind

# Primero verificar que 129A4 y 29C21 son thunks (jmp) y a donde saltan
# EN: First check that 129A4 and 29C21 are thunks (jmp) and where they jump
# ES: Devuelve el destino si la instrucción en rva es un JMP, o None.
# EN: Returns the target if the instruction at rva is a JMP, or None.
def thunk_target(rva):
    dec = Decoder(64, data[rva:rva+16], ip=IB+rva)
    ins = dec.decode()
    if ins.mnemonic == Mnemonic.JMP:
        return ins.near_branch_target - IB
    return None

print("thunk 0x129A4 ->", hex(thunk_target(0x129A4)) if thunk_target(0x129A4) else "NO JMP")
print("thunk 0x29C21 ->", hex(thunk_target(0x29C21)) if thunk_target(0x29C21) else "NO JMP")

TARGETS = {IB+0x129A4: "ctor_thunk_72D3B0", IB+0x29C21: "init_thunk_6E4F50"}

# Recorrer todo .text buscando CALL near a esos targets
# EN: Walk the whole .text looking for near CALLs to those targets
results = {k: [] for k in TARGETS}
off = TEXT_RVA
end = TEXT_RVA + TEXT_SZ
dec = Decoder(64, data[off:end], ip=IB+off)
for ins in dec:
    if ins.mnemonic == Mnemonic.CALL and ins.op0_kind == OpKind.NEAR_BRANCH64:
        tgt = ins.near_branch_target
        if tgt in TARGETS:
            rva = ins.ip - IB
            fc = func_containing(rva)
            results[tgt].append((rva, fc))

# ES: Informe por target.
# EN: Per-target report.
for tgt, name in TARGETS.items():
    print(f"\n=== CALLERS de {name} (target {hex(tgt-IB)}) ===")
    for rva, fc in results[tgt]:
        fcs = f"func {hex(fc[0])}..{hex(fc[1])}" if fc else "NO PDATA FUNC"
        print(f"  call @ {hex(rva)}  en {fcs}")
