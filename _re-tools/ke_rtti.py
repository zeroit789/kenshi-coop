# ES: Resuelve el nombre de clase RTTI (MSVC x64) de cada vtable pasada por RVA, junto con su
#     CompleteObjectLocator y TypeDescriptor. Uso: python ke_rtti.py <vtable_rva_hex> [...]
# EN: Resolves the RTTI class name (MSVC x64) of each vtable given by RVA, along with its
#     CompleteObjectLocator and TypeDescriptor. Usage: python ke_rtti.py <vtable_rva_hex> [...]

# Resuelve la clase RTTI a partir de una vtable RVA (x64 MSVC). READ-ONLY.
# EN: Resolves the RTTI class from a vtable RVA (x64 MSVC). READ-ONLY.
import sys, pefile, struct
EXE = r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
IB = 0x140000000
pe = pefile.PE(EXE, fast_load=True)
data = open(EXE,"rb").read()
# ES: Lectura de n bytes / u64 / u32 por RVA.
# EN: Read n bytes / u64 / u32 by RVA.
def rd(rva,n): 
    o=pe.get_offset_from_rva(rva); return data[o:o+n]
def u64(rva): return struct.unpack("<Q", rd(rva,8))[0]
def u32(rva): return struct.unpack("<I", rd(rva,4))[0]
# ES: Devuelve (nombre, COL, TD) de la vtable.
# EN: Returns (name, COL, TD) of the vtable.
def vtable_class(vt_rva):
    # COL en vt-8 (es un RVA? No: es puntero absoluto en .rdata -> qword)
    # EN: COL at vt-8 (is it an RVA? No: it is an absolute pointer in .rdata -> qword)
    col_ptr = u64(vt_rva-8)
    col_rva = col_ptr - IB
    # COL: [0]=sig [4]=offset [8]=cdOffset [0xC]=pTypeDescriptor(RVA) [0x10]=pClassHierarchy(RVA)
    # EN: COL: [0]=sig [4]=offset [8]=cdOffset [0xC]=pTypeDescriptor(RVA) [0x10]=pClassHierarchy(RVA)
    td_rva = u32(col_rva+0xC)
    # TypeDescriptor: [0]=vtable ptr [8]=spare [0x10]=name (.?AV...@@)
    # EN: TypeDescriptor: [0]=vtable ptr [8]=spare [0x10]=name (.?AV...@@)
    name = rd(td_rva+0x10, 128).split(b"\x00")[0].decode("ascii","replace")
    return name, col_rva, td_rva
for a in sys.argv[1:]:
    vt=int(a,16)
    try:
        name,col,td=vtable_class(vt)
        print(f"vtable 0x{vt:X}: {name}  (COL 0x{col:X}, TD 0x{td:X})")
    except Exception as e:
        print(f"vtable 0x{vt:X}: ERROR {e}")
