# ES: Lanzador del helper kdis: carga kdis.py desde la ruta antigua C:\Users\Zero\kdis.py (hay copia en
#     tools/re-scripts/kdis.py) y llama k.show(función, longitud, etiqueta).
#     Uso: python run_dis.py <rva_hex> [longitud_hex=0x180] [etiqueta]
# EN: Launcher for the kdis helper: loads kdis.py from the old path C:\Users\Zero\kdis.py (a copy lives
#     in tools/re-scripts/kdis.py) and calls k.show(function, length, label).
#     Usage: python run_dis.py <rva_hex> [length_hex=0x180] [label]

import importlib.util
spec = importlib.util.spec_from_file_location("kdis", r"C:\Users\Zero\kdis.py")
k = importlib.util.module_from_spec(spec); spec.loader.exec_module(k)
import sys
fn = int(sys.argv[1],16); ln = int(sys.argv[2],16) if len(sys.argv)>2 else 0x180
lbl = sys.argv[3] if len(sys.argv)>3 else ""
k.show(fn, ln, lbl)
