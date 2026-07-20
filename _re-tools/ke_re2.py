# Helper RE Kenshi Steam 1.0.68 - lectura/desensamblado solo-lectura
import pefile, sys
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000

_pe = None
_text_data = None
_text_rva = None
_text_size = None

def _load():
    global _pe,_text_data,_text_rva,_text_size
    if _pe: return
    _pe = pefile.PE(EXE, fast_load=True)
    for s in _pe.sections:
        nm = s.Name.rstrip(b'\x00')
        if nm == b'.text':
            _text_rva = s.VirtualAddress
            _text_size = s.Misc_VirtualSize
            _text_data = s.get_data()
    # full image for any-rva reads
    global _img
    _img = _pe.get_memory_mapped_image()

def read(rva, n):
    _load()
    return _img[rva:rva+n]

def disasm(rva, n=80, count=40):
    _load()
    data = _img[rva:rva+n]
    dec = Decoder(64, data, ip=IMAGE_BASE+rva)
    fmt = Formatter(FormatterSyntax.INTEL)
    out=[]
    i=0
    for ins in dec:
        if i>=count: break
        off = ins.ip - IMAGE_BASE
        b = _img[off:off+ins.len].hex()
        out.append(f"0x{off:X}: {b:<24} {fmt.format(ins)}")
        i+=1
    return "\n".join(out)

# seguir thunk jmp
def follow_thunk(rva):
    _load()
    data = _img[rva:rva+16]
    dec = Decoder(64, data, ip=IMAGE_BASE+rva)
    ins = next(iter(dec))
    if ins.mnemonic == Mnemonic.JMP:
        return ins.near_branch_target - IMAGE_BASE
    return None

if __name__=="__main__":
    cmd = sys.argv[1]
    if cmd=="d":
        rva=int(sys.argv[2],16)
        n=int(sys.argv[3]) if len(sys.argv)>3 else 120
        c=int(sys.argv[4]) if len(sys.argv)>4 else 50
        print(disasm(rva,n,c))
    elif cmd=="thunk":
        print(hex(follow_thunk(int(sys.argv[2],16))))

def vtslot(vtable_rva, slot_off):
    _load()
    import struct
    q = struct.unpack("<Q", _img[vtable_rva+slot_off:vtable_rva+slot_off+8])[0]
    thunk_rva = q - IMAGE_BASE
    real = follow_thunk(thunk_rva)
    return thunk_rva, real
