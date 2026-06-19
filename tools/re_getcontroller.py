# -*- coding: utf-8 -*-
# Resuelve el virtual slot +0x58 (getController) del Character y desensambla su cuerpo.
# OBJETIVO: confirmar que getController(char) devuelve un objeto cuyo +0x250 es el MISMO
# que SetControlledChar escribe (newChar+0x250=PI). Si getController==identidad (return this)
# o devuelve un sub-objeto fijo, el fix vale. Si devuelve otra cosa, el fix NO activa el gate.
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, FlowControl

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

def rva_to_off(rva):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if srva <= rva < srva + max(vsize, raw_size):
            d = rva - srva
            if d < raw_size:
                return raw_off + d
    return None

def read_q(rva):
    off = rva_to_off(rva)
    return struct.unpack_from("<Q", DATA, off)[0]

# La vtable del Character: el AI tick 0x5CCD90 hace mov rax,[rdi] (vtable) y call [rax+0x58].
# Necesitamos la vtable concreta. La obtenemos del RTTI de Character. Pero mas directo:
# el ctor de Character instala la vtable. Buscamos cualquier "lea reg,[vtblRVA]" cerca.
# Atajo: el AI tick rama viva opera sobre un Character generico; cualquier vtable de Character
# sirve. Probamos la vtable de Character via su RTTI ".?AVCharacter@@" no la tenemos aqui,
# asi que en su lugar desensamblamos el slot a partir de varias vtables candidatas conocidas.
# Mejor: escaneamos .rdata por vtables cuyo slot +0x58 apunte a una funcion pequena tipo getter.
# PERO lo mas fiable: el gate usa [rax+0x58] sobre el MISMO objeto del char (rax=[rdi]).
# Asi que getController es el virtual 11 (0x58/8) del Character. Resolvemos la vtable de
# Character localizando su ctor por el string de clase. Atajo pragmatico: el mod ya conoce
# la vtable de CharMovement (0x16FCC88). Para Character buscamos su vtable por patron del AI
# tick: la rama viva 0x5CD1C0 es codigo de Character::?, su 'this' es un Character.
#
# Enfoque directo y robusto: localizar TODAS las vtables en .rdata cuyo slot0 caiga en .text
# y cuyo +0x58 sea una funcion; luego para las que parezcan Character (tienen +0x1D8 y +0x268
# y +0xE8 validos como en el AI tick) desensamblar su +0x58.
RDATA = next(s for s in SECTIONS if s[0]==".rdata")
TEXT  = next(s for s in SECTIONS if s[0]==".text")
def in_text(rva): return TEXT[1] <= rva < TEXT[1]+TEXT[2]

# Character vtable: la conocemos por el AI tick. Vamos a buscar la vtable cuyo slot (0x1D8/8)=think
# apunte a algo, y (0x58/8)=getController. Demasiado costoso. Mejor: el mod tiene la vtable de
# Character en algun sitio. Buscamos en core/sdk. Atajo final: desensamblar getController a partir
# de la vtable de Character hallada por RTTI ".?AVCharacter@@".
# Buscar el string del type descriptor:
needle = b".?AVCharacter@@"
idx = DATA.find(needle)
print("RTTI .?AVCharacter@@ raw off:", hex(idx) if idx>=0 else "NO ENCONTRADO")
# raw off -> rva
def off_to_rva(off):
    for name, srva, vsize, raw_off, raw_size in SECTIONS:
        if raw_off <= off < raw_off+raw_size:
            return srva + (off - raw_off)
    return None
td_rva = off_to_rva(idx-0x10) if idx>=0 else None  # TypeDescriptor empieza 0x10 antes del nombre
print("TypeDescriptor RVA aprox:", hex(td_rva) if td_rva else None)

# Localizar la vtable: COL referencia el TD; vtable[-1] = COL. Escaneamos .rdata por punteros
# absolutos al TD (vtable_TypeDescriptor en el TypeDescriptor field del COL).
td_va = IMAGE_BASE + (idx-0x10) and None
# Demasiado fragil. Plan B robusto: desensamblar el slot +0x58 directamente NO hace falta si
# confirmamos por semantica: SetControlledChar escribe [newChar+0x250]=PI y el gate lee
# [getController(char)+0x250]. Si en MILES de chars normales el motor funciona, getController
# DEBE devolver el propio char (o un sub-objeto estable). Lo confirmamos viendo si ALGUN sitio
# del binario llama SetControlledChar y luego el char piensa. Pero lo definitivo es el slot.
#
# Resolvemos la vtable de Character por fuerza: buscamos en .text un "lea rcx,[vtblRVA]" seguido
# de patrones de ctor. Mas simple: el AI tick 0x5CCD90 ya nos dice que el 'this' (rdi) es Character
# y [rdi] su vtable. Buscamos en .rdata una vtable V tal que:
#   V[0x58/8] -> funcion getter pequena. Y desensamblamos esa.
# Para acotar: tomamos la vtable de Character desde el COL. Escaneo de COLs:
print("\n-- Buscando vtable de Character via COL/RTTI --")
# Escanear .rdata por qwords que apunten dentro de .text con un patron de vtable de Character:
# heuristica: una vtable de Character debe tener en offset 0x1D8 y 0x1E0 y 0x268 y 0xE8 punteros a .text.
rd_off = RDATA[3]; rd_size = RDATA[4]; rd_rva = RDATA[1]
candidates = []
step = 8
# limitar escaneo: buscar qword == puntero a 0x5CD... (la rama viva pertenece a Character::updateLogic)
# El slot que CONTIENE 0x5CCD90 (AI tick, +0xE8) identifica la vtable de Character.
target = IMAGE_BASE + 0x5CCD90
for off in range(rd_off, rd_off+rd_size-8, step):
    q = struct.unpack_from("<Q", DATA, off)[0]
    if q == target:
        slot_rva = off_to_rva(off)
        vtbl_base = slot_rva - 0xE8  # este slot es +0xE8
        candidates.append(vtbl_base)
print("vtables de Character candidatas (slot +0xE8 == 0x5CCD90):", [hex(c) for c in candidates])

for vt in candidates:
    s58 = read_q(vt + 0x58)
    getctl_rva = s58 - IMAGE_BASE
    print(f"\nvtable 0x{vt:X}: slot +0x58 (getController) = 0x{getctl_rva:X}")
    # desensamblar getController
    off = rva_to_off(getctl_rva)
    if off is None:
        print("  (fuera de rango)"); continue
    code = DATA[off:off+0x40]
    dec = Decoder(64, code, ip=IMAGE_BASE+getctl_rva)
    fmt = Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
    for instr in dec:
        rva = instr.ip - IMAGE_BASE
        raw = code[instr.ip-(IMAGE_BASE+getctl_rva):instr.ip-(IMAGE_BASE+getctl_rva)+instr.len]
        rawhex = " ".join(f"{b:02x}" for b in raw)
        print(f"  0x{rva:08X}  {rawhex:<20} {fmt.format(instr)}")
        if instr.mnemonic in (Mnemonic.RET, Mnemonic.INT3):
            break
