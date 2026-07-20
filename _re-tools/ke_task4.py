import ke_re as k
import struct, re

IB = 0x140000000
def u64(b): return struct.unpack('<Q', b[:8])[0]
def u32(b): return struct.unpack('<I', b[:4])[0]

def rtti_name(vt_rva):
    try:
        col_va = u64(k.bytes_at_rva(vt_rva-8, 8))
        if col_va < IB or col_va > IB+0x2000000: return None
        col_rva = col_va - IB
        col = k.bytes_at_rva(col_rva, 0x10)
        td_rva = u32(col[0xC:0x10])
        return k.read_string_near(td_rva+0x10, 80)
    except: return None

rvas = [0x74976,0xa96ec,0xb3b9a,0xb4c7e,0xdcf3d,0xf54da,0x10df78,0x10fb99,0x1abe78,0x1cc8d9,0x1f84a7,0x1f9137,0x1f9f69,0x1fa205,0x1fa8d1,0x1fb2ff,0x1fcb08,0x1fcc5d,0x206612,0x208f24,0x2094f4,0x209668,0x20c4d9,0x21f3f6,0x2235ac,0x22510a,0x225e58,0x226182,0x226e8f,0x22715b,0x227fde,0x232763,0x233e0a,0x2613a8,0x261eda,0x267ffd,0x3c8b32,0x3ce224,0x40bad6,0x42f392,0x44b3c0,0x471f1b,0x47add6,0x47ae64,0x48d823,0x492de1,0x49c3f0,0x49f6d4,0x5005fa,0x500656,0x52c28f,0x535990,0x535d01,0x5b97dd,0x5bd2b3,0x5bd467,0x5e8aad,0x646aa2,0x66b64b,0x6ccd75,0x6ccd8d,0x72a9a3,0x72b210,0x72b32a,0x72d4ed,0x7dc0aa,0x7dc0cb,0x7f6c56,0x7fa635,0x886ee1,0x913c87,0x917644,0xb01f6f,0xb02497,0xb02b91,0xb02eba,0xb08680,0xb08e5d,0xb3f8a4,0xb3f97c,0xb64b81,0xb64bea,0xb64cc5,0xb64ef4,0xb65366,0xb65657,0xb656e5,0xb65979,0xb65c36,0xb65d87,0xb6601e,0xb6604f,0xb66061,0xb66514,0xb6667a,0xb6668d,0xb7cee4,0xb7e213,0xb7e25b,0xb7ea24,0xb7fdf8,0xc1902a,0xc196f7,0xe11714,0xe11784,0xf005fe]

mov_re = re.compile(r'mov \[(\w+)\+0E8h\],(\w+)')

# Para cada escritura: identificar srcreg, y trazar hacia atras la ultima def de srcreg.
# Buscamos patrones:
#   call X ; ... mov srcreg,rax  -> origen = retorno de call X (posible alocador/ctor)
#   mov srcreg,[dst+off]         -> copiado de otro campo (ya existe)
#   mov srcreg,[reg+off]         -> lectura
# Imprimimos las ~10 lineas previas a la escritura para clasificar.
for rva in rvas:
    start = rva - 0x60
    try:
        block = k.disasm(start, 0x60+8, 40)
    except Exception as e:
        print(f"0x{rva:X} ERR {e}"); continue
    lines = block.splitlines()
    # localizar indice de la linea objetivo
    idx = None; srcreg=None; dstreg=None
    for i,ln in enumerate(lines):
        m = mov_re.search(ln)
        if m and (hex(rva)[2:].lower() in ln.lower()):
            idx=i; dstreg,srcreg=m.group(1),m.group(2); break
    if idx is None:
        # fallback: tomar ultima coincidencia
        for i,ln in enumerate(lines):
            m=mov_re.search(ln)
            if m: idx=i; dstreg,srcreg=m.group(1),m.group(2)
    prev = lines[max(0,idx-8):idx] if idx is not None else []
    # detectar si justo antes hay call (origen=retorno) o mov src,[..]
    origin="?"
    for ln in reversed(prev):
        low=ln.lower()
        # def de srcreg
        if re.search(rf'\b{srcreg}\b', low):
            if 'call' in low: origin="CALL_RET"; break
            mm=re.search(rf'mov {srcreg},(.+)', low)
            if mm: origin="MOV<-"+mm.group(1).strip(); break
            mm=re.search(rf'lea {srcreg},(.+)', low)
            if mm: origin="LEA<-"+mm.group(1).strip(); break
            mm=re.search(rf'(\w*call\w*) ', low)
    # Tambien: hubo un 'call' en las 8 lineas previas?
    callprev = [ln.split('0x')[-1] for ln in prev if 'call ' in ln.lower()]
    callstr=""
    for ln in prev:
        if 'call' in ln.lower():
            # extraer destino del call
            cm=re.search(r'call\s+(0x[0-9A-Fa-f]+|\w+)', ln)
            if cm: callstr+= cm.group(1)+" "
    print(f"0x{rva:X} src={srcreg} dst={dstreg} origin[{origin}] calls_in_window=[{callstr.strip()}]")
