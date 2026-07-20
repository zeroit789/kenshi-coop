# ke_re.py — helper READ-ONLY de RE para Kenshi Steam 1.0.68
# Mapea RVA->bytes y desensambla con iced-x86. NO modifica el binario.
import sys, pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax

EXE = r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
IMAGE_BASE = 0x140000000

_pe = None
_data = None
def _load():
    global _pe, _data
    if _pe is None:
        _pe = pefile.PE(EXE, fast_load=True)
        with open(EXE, "rb") as f:
            _data = f.read()
    return _pe, _data

def rva_to_off(rva):
    pe, _ = _load()
    return pe.get_offset_from_rva(rva)

def bytes_at_rva(rva, n=32):
    pe, data = _load()
    off = pe.get_offset_from_rva(rva)
    return data[off:off+n]

def hexbytes(rva, n=32):
    return " ".join(f"{b:02X}" for b in bytes_at_rva(rva, n))

def disasm(rva, n=64, count=12):
    b = bytes_at_rva(rva, n)
    dec = Decoder(64, b, ip=IMAGE_BASE+rva)
    fmt = Formatter(FormatterSyntax.INTEL)
    out = []
    i = 0
    for ins in dec:
        if i >= count: break
        off = ins.ip - (IMAGE_BASE+rva)
        raw = " ".join(f"{x:02X}" for x in b[off:off+ins.len])
        out.append(f"  +{off:02X} {IMAGE_BASE+ins.ip-IMAGE_BASE:>0} 0x{ins.ip:X}: {fmt.format(ins):<40} ; {raw}")
        i += 1
    return "\n".join(out)

def find_pattern(pat):
    # pat: "48 8B C4 ?? 55" -> lista de matches (RVAs). ?? o ? = wildcard
    pe, data = _load()
    toks = pat.split()
    mask = []
    for t in toks:
        if t in ("??","?"): mask.append(None)
        else: mask.append(int(t,16))
    # buscar solo en .text
    text = None
    for s in pe.sections:
        if b".text" in s.Name:
            text = s; break
    start = text.PointerToRawData
    end = start + text.SizeOfRawData
    base_rva = text.VirtualAddress
    matches = []
    n = len(mask)
    i = start
    seg = data
    while i < end-n:
        ok = True
        for j in range(n):
            m = mask[j]
            if m is not None and seg[i+j] != m:
                ok = False; break
        if ok:
            file_off = i
            rva = base_rva + (i - start)
            matches.append(rva)
            if len(matches) > 5: break
        i += 1
    return matches

def prev_bytes(rva, n=8):
    pe, data = _load()
    off = pe.get_offset_from_rva(rva)
    return " ".join(f"{b:02X}" for b in data[off-n:off])

def read_string_near(rva, n=64):
    b = bytes_at_rva(rva, n)
    s = b.split(b"\x00")[0]
    try: return s.decode("ascii","replace")
    except: return repr(s)

if __name__ == "__main__":
    cmd = sys.argv[1]
    rva = int(sys.argv[2],16)
    if cmd == "hex":
        n = int(sys.argv[3]) if len(sys.argv)>3 else 32
        print(f"RVA 0x{rva:X} prev8=[{prev_bytes(rva)}]")
        print(hexbytes(rva,n))
    elif cmd == "dis":
        n = int(sys.argv[3]) if len(sys.argv)>3 else 80
        print(f"RVA 0x{rva:X} prev8=[{prev_bytes(rva)}]")
        print(disasm(rva,n,16))
    elif cmd == "str":
        print(read_string_near(rva))
