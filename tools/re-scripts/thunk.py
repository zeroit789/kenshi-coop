# ES: Sigue las cadenas de thunks jmp desde varios destinos de call del encolador de órdenes (primer gate,
#     el que encola de verdad, un getter...) e imprime cada salto hasta la función final. Carga kdis desde
#     la ruta antigua C:\Users\Zero\kdis.py. Uso: python thunk.py
# EN: Follows jmp thunk chains from several call targets of the order enqueuer (first gate, the real
#     enqueue, a getter...) printing every hop to the final function. Loads kdis from the old path
#     C:\Users\Zero\kdis.py. Usage: python thunk.py

import importlib.util
spec = importlib.util.spec_from_file_location("kdis", r"C:\Users\Zero\kdis.py")
k = importlib.util.module_from_spec(spec); spec.loader.exec_module(k)
ib = k.image_base
# ES: Imprime la instrucción en va y, si es un jmp directo, la sigue recursivamente; devuelve la VA final.
# EN: Prints the instruction at va and, if it is a direct jmp, follows it recursively; returns the final VA.
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
