import ke_re as k
import struct
IB=0x140000000
def u64(b): return struct.unpack('<Q',b[:8])[0]
def u32(b): return struct.unpack('<I',b[:4])[0]
def rtti_name(vt):
    try:
        col=u64(k.bytes_at_rva(vt-8,8))
        if col<IB or col>IB+0x2000000: return None
        c=k.bytes_at_rva(col-IB,0x10); td=u32(c[0xC:0x10])
        return k.read_string_near(td+0x10,80)
    except: return None

# 0x10DF78: singleton [0x142133308] -> obj +0x60 -> vtable -> call vt[0x1D0].
# El singleton en 0x142133308 (data). No podemos resolver runtime. Pero veamos
# si esta funcion contenedora instala vtable AnimationClass. Encontrar prologo.
# Buscar hacia atras el inicio de funcion (int3 padding + push/sub rsp).
def find_func_start(rva, maxback=0x2000):
    raw = k.bytes_at_rva(rva-maxback, maxback)
    # buscar ultima secuencia CC CC seguida de un prologo plausible antes de rva
    best=None
    for i in range(len(raw)-1):
        if raw[i]==0xCC and raw[i+1]!=0xCC:
            cand = rva-maxback+i+1
            best=cand
    return best

for rva in [0x10DF78, 0x72A9A3, 0x72B210, 0x7DC0AA]:
    fs=find_func_start(rva)
    if fs:
        blk=k.disasm(fs, min(rva-fs+12, 0x1500), 700)
        hasAnim=('16f4588' in blk.lower()) or ('16f10e8' in blk.lower())
        has448='448h' in blk
        # contar refs a vtables conocidas Task
        hasTasker=('16bdc68' in blk.lower())
        hasMelee=('16be448' in blk.lower())
        print(f"0x{rva:X} funcstart~0x{fs:X} len=0x{rva-fs:X} | anim={hasAnim} c448={has448} tasker={hasTasker} melee={hasMelee}")
    else:
        print(f"0x{rva:X} no funcstart")
