# ES: Sigue cadenas de thunks jmp desde varias direcciones absolutas (helper rel r8d=3/4, rama isAlly,
#     delegado final...) imprimiendo cada salto. Usa el desensamblador de kdis.py (atributo md,
#     probablemente Capstone) cargado desde la ruta antigua C:\Users\Zero\kdis.py. Uso: python thunk2.py
# EN: Follows jmp thunk chains from several absolute addresses (rel helper r8d=3/4, isAlly branch, final
#     delegate...) printing every hop. Uses kdis.py's disassembler (md attribute, probably Capstone)
#     loaded from the old path C:\Users\Zero\kdis.py. Usage: python thunk2.py

import importlib.util
spec = importlib.util.spec_from_file_location("kdis", r"C:\Users\Zero\kdis.py")
k = importlib.util.module_from_spec(spec); spec.loader.exec_module(k)
ib = k.image_base
# ES: Imprime la instrucción en va y, si es un jmp directo, sigue recursivamente.
# EN: Prints the instruction at va and, if it is a direct jmp, follows it recursively.
def resolve(va, depth=0):
    rva = va - ib; off = k.rva_to_off(rva)
    insn = next(k.md.disasm(k.data[off:off+16], va))
    print("  "*depth + f"@0x{va:X}: {insn.mnemonic} {insn.op_str}")
    if insn.mnemonic=='jmp' and insn.op_str.startswith('0x'):
        return resolve(int(insn.op_str,16), depth+1)
    return va
for name,va in [("helper rel (r8d=3/4) 0x140022971",0x140022971),
                ("0x14001C7B5 (en rama isAlly)",0x14001C7B5),
                ("0x140033D8E",0x140033D8E),
                ("0x140040674 (delegado final)",0x140040674)]:
    print("===", name); resolve(va); print()
