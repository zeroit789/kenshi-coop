# ES: Helper principal de la serie ke_* (lo importan muchos scripts como "import ke_re as k"): carga
#     kenshi_x64.exe una sola vez y ofrece lectura de bytes por RVA, hexdump, desensamblado,
#     búsqueda de patrones AOB en .text (máx. 6 coincidencias) y lectura de cadenas.
#     Uso directo: python ke_re.py hex|dis|str <rva_hex> [n]
# EN: Main helper of the ke_* series (many scripts import it as "import ke_re as k"): loads
#     kenshi_x64.exe once and offers byte reads by RVA, hexdump, disassembly, AOB pattern search in
#     .text (max 6 matches) and string reads.
#     Direct use: python ke_re.py hex|dis|str <rva_hex> [n]

# ke_re.py — helper READ-ONLY de RE para Kenshi Steam 1.0.68
# Mapea RVA->bytes y desensambla con iced-x86. NO modifica el binario.
# EN: ke_re.py - READ-ONLY RE helper for Kenshi Steam 1.0.68
#     Maps RVA->bytes and disassembles with iced-x86. Does NOT modify the binary.
import sys, pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax

EXE = r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
IMAGE_BASE = 0x140000000

# ES: Caché del PE y de los bytes crudos del fichero (se cargan en la primera llamada).
# EN: Cache of the PE and the raw file bytes (loaded on first call).
_pe = None
_data = None
def _load():
    global _pe, _data
    if _pe is None:
        _pe = pefile.PE(EXE, fast_load=True)
        with open(EXE, "rb") as f:
            _data = f.read()
    return _pe, _data

# ES: RVA -> offset en el fichero.
# EN: RVA -> file offset.
def rva_to_off(rva):
    pe, _ = _load()
    return pe.get_offset_from_rva(rva)

# ES: n bytes crudos a partir de un RVA.
# EN: n raw bytes starting at an RVA.
def bytes_at_rva(rva, n=32):
    pe, data = _load()
    off = pe.get_offset_from_rva(rva)
    return data[off:off+n]

# ES: Los mismos bytes en hexadecimal separados por espacios.
# EN: The same bytes as space-separated hex.
def hexbytes(rva, n=32):
    return " ".join(f"{b:02X}" for b in bytes_at_rva(rva, n))

# ES: Desensambla n bytes desde rva (máx. count instrucciones) con offset, dirección y bytes.
# EN: Disassembles n bytes from rva (max count instructions) with offset, address and bytes.
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

# ES: Busca un patrón AOB en .text y devuelve sus RVAs (se detiene tras 6 coincidencias).
# EN: Searches .text for an AOB pattern and returns its RVAs (stops after 6 matches).
def find_pattern(pat):
    # pat: "48 8B C4 ?? 55" -> lista de matches (RVAs). ?? o ? = wildcard
    # EN: pat: "48 8B C4 ?? 55" -> list of matches (RVAs). ?? or ? = wildcard
    pe, data = _load()
    toks = pat.split()
    mask = []
    for t in toks:
        if t in ("??","?"): mask.append(None)
        else: mask.append(int(t,16))
    # buscar solo en .text
    # EN: search only in .text
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

# ES: Los n bytes anteriores a un RVA (para ver el relleno previo a una función).
# EN: The n bytes before an RVA (to see the padding before a function).
def prev_bytes(rva, n=8):
    pe, data = _load()
    off = pe.get_offset_from_rva(rva)
    return " ".join(f"{b:02X}" for b in data[off-n:off])

# ES: Cadena ASCII terminada en 0 a partir de un RVA.
# EN: NUL-terminated ASCII string starting at an RVA.
def read_string_near(rva, n=64):
    b = bytes_at_rva(rva, n)
    s = b.split(b"\x00")[0]
    try: return s.decode("ascii","replace")
    except: return repr(s)

# ES: Modo línea de comandos: hex / dis / str.
# EN: Command-line mode: hex / dis / str.
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
