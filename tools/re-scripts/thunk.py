import importlib.util
spec = importlib.util.spec_from_file_location("kdis", r"C:\Users\Zero\kdis.py")
k = importlib.util.module_from_spec(spec); spec.loader.exec_module(k)
ib = k.image_base
def resolve(va, depth=0):
    rva = va - ib
    off = k.rva_to_off(rva)
    code = k.data[off:off+16]
    insn = next(k.md.disasm(code, va))
    print("  "*depth + f"@0x{va:X}: {insn.mnemonic} {insn.op_str}")
    if insn.mnemonic == 'jmp' and insn.op_str.startswith('0x'):
        tgt = int(insn.op_str,16)
        return resolve(tgt, depth+1)
    return va
print("=== call 0x140015F82 (primer gate del encolador) resuelve a:")
final = resolve(0x140015F82)
print("  FINAL =", hex(final), "RVA", hex(final-ib))
print("\n=== call 0x14001CD69 (encola de verdad) resuelve a:")
resolve(0x14001CD69)
print("\n=== call 0x1400014B5 (segundo path, getter) resuelve a:")
resolve(0x1400014B5)
print("\n=== call 0x14004D9C8 resuelve a:")
resolve(0x14004D9C8)
