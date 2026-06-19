# -*- coding: utf-8 -*-
# Onyx 2026-06-19: Localiza el literal "204-gamedata.base" en .rdata y hace xref completo
# de todas las instrucciones `lea reg,[rip+disp]` en .text que apuntan a ese RVA.
# Tambien busca referencias en .data/.rdata (qword punteros). Para cada xref desensambla
# contexto y trata de identificar la funcion contenedora via .pdata.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
IMAGE_BASE = 0x140000000
with open(EXE, "rb") as f:
    DATA = f.read()

e_lfanew = struct.unpack_from("<I", DATA, 0x3C)[0]
coff = e_lfanew + 4
num_sec = struct.unpack_from("<H", DATA, coff + 2)[0]
opt_size = struct.unpack_from("<H", DATA, coff + 16)[0]
opt = coff + 20
sec_off = opt + opt_size
SECTIONS = []
for i in range(num_sec):
    o = sec_off + i*40
    name = DATA[o:o+8].rstrip(b"\x00").decode("ascii","ignore")
    vsize = struct.unpack_from("<I", DATA, o+8)[0]
    rva = struct.unpack_from("<I", DATA, o+12)[0]
    raw_size = struct.unpack_from("<I", DATA, o+16)[0]
    raw_off = struct.unpack_from("<I", DATA, o+20)[0]
    SECTIONS.append((name, rva, vsize, raw_off, raw_size))

def sec_of(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            return name, srva, vsize, raw_off, raw_size
    return None

def rva_to_off(rva):
    s = sec_of(rva)
    if not s: return None
    name, srva, vsize, raw_off, raw_size = s
    d = rva - srva
    if d < raw_size:
        return raw_off + d
    return None

def get_text():
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if name == ".text":
            return srva, raw_off, raw_size
    return None

# 1) Localizar el literal exacto "204-gamedata.base\x00" en .rdata
needle = b"204-gamedata.base\x00"
str_rva = None
# buscar en todo el fichero, reportar RVA correspondiente
idx = -1
while True:
    idx = DATA.find(needle, idx+1)
    if idx == -1: break
    # convertir offset->rva
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if raw_off <= idx < raw_off + raw_size:
            cand_rva = srva + (idx - raw_off)
            print(f"[STR] '204-gamedata.base' en file_off=0x{idx:X} -> RVA 0x{cand_rva:X} (sec {name})")
            if name == ".rdata" and str_rva is None:
                str_rva = cand_rva
            break

if str_rva is None:
    print("No se encontro el literal en .rdata"); raise SystemExit
print(f"\n[+] Usando str_rva = 0x{str_rva:X} (VA 0x{IMAGE_BASE+str_rva:X})\n")

target_va = IMAGE_BASE + str_rva

# 2) Escanear .text para lea reg,[rip+disp] -> target_va, y tambien mov reg,[rip+disp] / push etc.
srva, raw_off, raw_size = get_text()
code = DATA[raw_off:raw_off+raw_size]
dec = Decoder(64, code, ip=IMAGE_BASE+srva)
fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""

xrefs = []
for instr in dec:
    # iced calcula memory_displacement absoluto para RIP-relative
    if instr.memory_base == Register.RIP:
        disp = instr.memory_displacement
        if disp == target_va:
            rva = instr.ip - IMAGE_BASE
            xrefs.append((rva, instr.mnemonic, fmt.format(instr)))

print(f"[+] {len(xrefs)} xref(s) RIP-relativos al literal en .text:\n")
for rva, mn, txt in xrefs:
    print(f"  RVA 0x{rva:08X}  {txt}")

# 3) Tambien buscar punteros absolutos (qword == target_va) en .rdata/.data
print("\n[+] Buscando punteros absolutos (qword == VA del literal) en todo el binario:")
qneedle = struct.pack("<Q", target_va)
i = -1
found_ptr = 0
while True:
    i = DATA.find(qneedle, i+1)
    if i == -1: break
    for name, s2, vsize, ro, rs in SECTIONS:
        if ro <= i < ro+rs:
            prva = s2 + (i-ro)
            print(f"  qword-ptr en RVA 0x{prva:X} (sec {name})")
            found_ptr += 1
            break
if found_ptr == 0:
    print("  (ninguno)")

# 4) .pdata para localizar funcion contenedora de cada xref
def load_pdata():
    for name, srva2, vsize, ro, rs in SECTIONS:
        if name == ".pdata":
            return srva2, ro, rs
    return None
pd = load_pdata()
funcs = []
if pd:
    psrva, pro, prs = pd
    n = prs // 12
    for k in range(n):
        b = pro + k*12
        beg = struct.unpack_from("<I", DATA, b)[0]
        end = struct.unpack_from("<I", DATA, b+4)[0]
        if beg or end:
            funcs.append((beg, end))
    funcs.sort()

def func_of(rva):
    lo, hi = 0, len(funcs)-1
    res = None
    while lo <= hi:
        mid = (lo+hi)//2
        b, e = funcs[mid]
        if b <= rva < e:
            return b, e
        if rva < b: hi = mid-1
        else: lo = mid+1
    return None

print("\n[+] Funcion contenedora (via .pdata) de cada xref:")
for rva, mn, txt in xrefs:
    fo = func_of(rva)
    if fo:
        print(f"  xref RVA 0x{rva:08X}  ->  func 0x{fo[0]:08X}..0x{fo[1]:08X}  (off +0x{rva-fo[0]:X})")
    else:
        print(f"  xref RVA 0x{rva:08X}  ->  (sin .pdata; func no resuelta)")
