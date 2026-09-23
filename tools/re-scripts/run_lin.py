# ES: Desensamblado lineal (sin parar en ret) de longitud fija con el helper kdis cargado desde la ruta
#     antigua C:\Users\Zero\kdis.py (la copia versionada está en esta carpeta).
#     Uso: python run_lin.py <rva_hex> [longitud_hex=0x180] [etiqueta]
# EN: Fixed-length linear disassembly (does not stop at ret) with the kdis helper loaded from the old
#     path C:\Users\Zero\kdis.py (the versioned copy is in this folder).
#     Usage: python run_lin.py <rva_hex> [length_hex=0x180] [label]

import importlib.util
spec = importlib.util.spec_from_file_location("kdis", r"C:\Users\Zero\kdis.py")
k = importlib.util.module_from_spec(spec); spec.loader.exec_module(k)
import sys
fn = int(sys.argv[1],16); ln = int(sys.argv[2],16) if len(sys.argv)>2 else 0x180
lbl = sys.argv[3] if len(sys.argv)>3 else ""
ib = k.image_base
print(f"\n===== {lbl} @ RVA 0x{fn:X} (VA 0x{ib+fn:X}) linear {ln:#x} =====")
off = k.rva_to_off(fn)
code = k.data[off:off+ln]
for insn in k.md.disasm(code, ib+fn):
    b=' '.join(f'{x:02X}' for x in insn.bytes)
    print(f"0x{insn.address-ib:06X}: {b:<30} {insn.mnemonic} {insn.op_str}")
