import importlib.util, struct
spec = importlib.util.spec_from_file_location("kdis", r"C:\Users\Zero\kdis.py")
k = importlib.util.module_from_spec(spec); spec.loader.exec_module(k)
for rva in (0x1683170, 0x16CCD2C):
    b = k.read_rva(rva,4)
    f = struct.unpack('<f', b)[0]
    print(f"rdata 0x{rva:X} = bytes {b.hex()} = float {f}")
# Confirmar cálculo RIP de 0x6B2698 y 0x6B2703
print("0x6B2698 comiss target =", hex(0x6B269F + 0xFD0AD1))
print("0x6B2703 movss  target =", hex(0x6B270B + 0x101A621))
