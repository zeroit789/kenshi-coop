# ES: Variante del helper ke_re: carga la imagen mapeada (lectura por RVA) y ofrece read(),
#     disasm(), follow_thunk() y vtslot() (slot de vtable + destino real tras el thunk).
#     Uso directo: python ke_re2.py d <rva_hex> [n] [count] | python ke_re2.py thunk <rva_hex>
# EN: Variant of the ke_re helper: loads the mapped image (read by RVA) and offers read(), disasm(),
#     follow_thunk() and vtslot() (vtable slot + real target after the thunk).
#     Direct use: python ke_re2.py d <rva_hex> [n] [count] | python ke_re2.py thunk <rva_hex>

# Helper RE Kenshi Steam 1.0.68 - lectura/desensamblado solo-lectura
# EN: Kenshi Steam 1.0.68 RE helper - read-only reads/disassembly
import pefile, sys
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000

_pe = None
_text_data = None
_text_rva = None
_text_size = None

# ES: Carga perezosa del PE, de .text y de la imagen completa mapeada (_img).
# EN: Lazy load of the PE, .text and the full mapped image (_img).
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

# ES: n bytes a partir de un RVA.
# EN: n bytes starting at an RVA.
def read(rva, n):
    _load()
    return _img[rva:rva+n]

# ES: Desensambla n bytes desde rva (máx. count instrucciones) con bytes en hex.
# EN: Disassembles n bytes from rva (max count instructions) with hex bytes.
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
# EN: follow a jmp thunk: returns the target if the instruction at rva is a JMP, or None
def follow_thunk(rva):
    _load()
    data = _img[rva:rva+16]
    dec = Decoder(64, data, ip=IMAGE_BASE+rva)
    ins = next(iter(dec))
    if ins.mnemonic == Mnemonic.JMP:
        return ins.near_branch_target - IMAGE_BASE
    return None

# ES: Modo línea de comandos: d / thunk.
# EN: Command-line mode: d / thunk.
if __name__=="__main__":
    cmd = sys.argv[1]
    if cmd=="d":
        rva=int(sys.argv[2],16)
        n=int(sys.argv[3]) if len(sys.argv)>3 else 120
        c=int(sys.argv[4]) if len(sys.argv)>4 else 50
        print(disasm(rva,n,c))
    elif cmd=="thunk":
        print(hex(follow_thunk(int(sys.argv[2],16))))

# ES: Lee el slot slot_off de la vtable y devuelve (RVA del thunk, destino real o None).
# EN: Reads slot slot_off of the vtable and returns (thunk RVA, real target or None).
def vtslot(vtable_rva, slot_off):
    _load()
    import struct
    q = struct.unpack("<Q", _img[vtable_rva+slot_off:vtable_rva+slot_off+8])[0]
    thunk_rva = q - IMAGE_BASE
    real = follow_thunk(thunk_rva)
    return thunk_rva, real
