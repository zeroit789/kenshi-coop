# Resuelve la clase RTTI a partir de una vtable RVA (x64 MSVC). READ-ONLY.
import sys, pefile, struct
EXE = r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
IB = 0x140000000
pe = pefile.PE(EXE, fast_load=True)
data = open(EXE,"rb").read()
def rd(rva,n): 
    o=pe.get_offset_from_rva(rva); return data[o:o+n]
def u64(rva): return struct.unpack("<Q", rd(rva,8))[0]
def u32(rva): return struct.unpack("<I", rd(rva,4))[0]
def vtable_class(vt_rva):
    # COL en vt-8 (es un RVA? No: es puntero absoluto en .rdata -> qword)
    col_ptr = u64(vt_rva-8)
    col_rva = col_ptr - IB
    # COL: [0]=sig [4]=offset [8]=cdOffset [0xC]=pTypeDescriptor(RVA) [0x10]=pClassHierarchy(RVA)
    td_rva = u32(col_rva+0xC)
    # TypeDescriptor: [0]=vtable ptr [8]=spare [0x10]=name (.?AV...@@)
    name = rd(td_rva+0x10, 128).split(b"\x00")[0].decode("ascii","replace")
    return name, col_rva, td_rva
for a in sys.argv[1:]:
    vt=int(a,16)
    try:
        name,col,td=vtable_class(vt)
        print(f"vtable 0x{vt:X}: {name}  (COL 0x{col:X}, TD 0x{td:X})")
    except Exception as e:
        print(f"vtable 0x{vt:X}: ERROR {e}")
