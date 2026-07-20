exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind

# Primero verificar que 129A4 y 29C21 son thunks (jmp) y a donde saltan
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

for tgt, name in TARGETS.items():
    print(f"\n=== CALLERS de {name} (target {hex(tgt-IB)}) ===")
    for rva, fc in results[tgt]:
        fcs = f"func {hex(fc[0])}..{hex(fc[1])}" if fc else "NO PDATA FUNC"
        print(f"  call @ {hex(rva)}  en {fcs}")
