import ke_re as k
import struct, re

IB = 0x140000000
def u64(b): return struct.unpack('<Q', b[:8])[0]
def u32(b): return struct.unpack('<I', b[:4])[0]

# Vtables de la jerarquia Task/Tasker. Sabemos Tasker=0x16BDC68, Melee=0x16BE448.
# La jerarquia Task suele estar contigua en .rdata. Vamos a detectar cualquier vtable
# en rango 0x16BD000-0x16BF000 (zona Task/Tasker) como "sospechosa".
def rtti_name(vt_rva):
    try:
        col_va = u64(k.bytes_at_rva(vt_rva-8, 8))
        if col_va < IB or col_va > IB+0x2000000: return None
        col_rva = col_va - IB
        col = k.bytes_at_rva(col_rva, 0x10)
        td_rva = u32(col[0xC:0x10])
        name = k.read_string_near(td_rva+0x10, 80)
        return name
    except Exception as e:
        return None

rvas = [0x74976,0xa96ec,0xb3b9a,0xb4c7e,0xdcf3d,0xf54da,0x10df78,0x10fb99,0x1abe78,0x1cc8d9,0x1f84a7,0x1f9137,0x1f9f69,0x1fa205,0x1fa8d1,0x1fb2ff,0x1fcb08,0x1fcc5d,0x206612,0x208f24,0x2094f4,0x209668,0x20c4d9,0x21f3f6,0x2235ac,0x22510a,0x225e58,0x226182,0x226e8f,0x22715b,0x227fde,0x232763,0x233e0a,0x2613a8,0x261eda,0x267ffd,0x3c8b32,0x3ce224,0x40bad6,0x42f392,0x44b3c0,0x471f1b,0x47add6,0x47ae64,0x48d823,0x492de1,0x49c3f0,0x49f6d4,0x5005fa,0x500656,0x52c28f,0x535990,0x535d01,0x5b97dd,0x5bd2b3,0x5bd467,0x5e8aad,0x646aa2,0x66b64b,0x6ccd75,0x6ccd8d,0x72a9a3,0x72b210,0x72b32a,0x72d4ed,0x7dc0aa,0x7dc0cb,0x7f6c56,0x7fa635,0x886ee1,0x913c87,0x917644,0xb01f6f,0xb02497,0xb02b91,0xb02eba,0xb08680,0xb08e5d,0xb3f8a4,0xb3f97c,0xb64b81,0xb64bea,0xb64cc5,0xb64ef4,0xb65366,0xb65657,0xb656e5,0xb65979,0xb65c36,0xb65d87,0xb6601e,0xb6604f,0xb66061,0xb66514,0xb6667a,0xb6668d,0xb7cee4,0xb7e213,0xb7e25b,0xb7ea24,0xb7fdf8,0xc1902a,0xc196f7,0xe11714,0xe11784,0xf005fe]

# regex para extraer destino y fuente del mov [reg+0xE8],reg
mov_re = re.compile(r'mov \[(\w+)\+0E8h\],(\w+)')
# lea reg, [rip+...] -> direccion absoluta; ke_re disasm ya resuelve a 0x140...
lea_abs = re.compile(r'lea (\w+),\[(0x[0-9A-Fa-f]+)\]')
movabs_re = re.compile(r'mov (\w+),0x([0-9A-Fa-f]+)')

# Para cada escritura, mirar 0x90 bytes antes y detectar referencias a vtables Task-range
for rva in rvas:
    try:
        # desensamblar contexto previo. start = rva-0x90
        start = rva - 0x90
        block = k.disasm(start, 0x90+12, 60)
    except Exception as e:
        print(f"0x{rva:X}: ERRBLK {e}"); continue
    lines = block.splitlines()
    # encontrar la linea de la escritura objetivo
    srcreg = None; dstreg = None
    for ln in lines:
        m = mov_re.search(ln)
        if m and (f'0x{rva:X}'.lower() in ln.lower()):
            dstreg, srcreg = m.group(1), m.group(2)
            break
    # buscar vtables referenciadas en el bloque (lea con direccion absoluta a .rdata>0x1600000)
    vtrefs = []
    for ln in lines:
        for mm in re.finditer(r'0x14([0-9A-Fa-f]{6,7})', ln):
            val = int('0x14'+mm.group(1),16)
            r = val - IB
            if 0x1600000 <= r <= 0x16FFFFF:
                nm = rtti_name(r)
                if nm:
                    vtrefs.append((r,nm))
    # marcar si hay vtable de rango task 0x16BD000-0x16BF000
    tasky = [v for v in vtrefs if 0x16BD000 <= v[0] <= 0x16BFFFF]
    flag = ""
    if tasky: flag = " <<< TASK-RANGE VTABLE"
    if vtrefs:
        uniq = sorted(set(vtrefs))
        vts = ", ".join(f"0x{r:X}={n}" for r,n in uniq)
        print(f"0x{rva:X} src={srcreg} dst={dstreg} | VTREFS: {vts}{flag}")
