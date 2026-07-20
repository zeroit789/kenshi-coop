import importlib.util
spec = importlib.util.spec_from_file_location("kdis", r"C:\Users\Zero\kdis.py")
k = importlib.util.module_from_spec(spec); spec.loader.exec_module(k)
import sys
fn = int(sys.argv[1],16); ln = int(sys.argv[2],16) if len(sys.argv)>2 else 0x180
lbl = sys.argv[3] if len(sys.argv)>3 else ""
k.show(fn, ln, lbl)
