# ES: Para cada escritura "mov [reg+0xE8], reg" en .text, mira una ventana de ~0x700 bytes antes
#     (y 0x40 después) y la marca si hay accesos a +0x448 o referencias RIP-relativas a las vtables
#     AnimationClass (0x16F10E8 / 0x16F1268). Imprime solo las marcadas. Uso: python ke_e8scan2.py
# EN: For each "mov [reg+0xE8], reg" write in .text, looks at a window of ~0x700 bytes before (and
#     0x40 after) and flags it if there are +0x448 accesses or RIP-relative references to the
#     AnimationClass vtables (0x16F10E8 / 0x16F1268). Prints only flagged ones. Usage: python ke_e8scan2.py

# Para cada 'mov [reg+0xE8], regPtr' en .text, reporta y marca si la funcion contenedora referencia
# vtable AnimationClass (0x16F10E8 / 0x16F1268) o usa offset 0x448, escaneando ~0x400 bytes alrededor.
# EN: For each 'mov [reg+0xE8], regPtr' in .text, report and flag whether the containing function references
#     the AnimationClass vtable (0x16F10E8 / 0x16F1268) or uses offset 0x448, scanning ~0x400 bytes around
#     (the code actually uses 0x700 before and 0x40 after).
import pefile, struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, OpKind, Register
EXE=r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"; IB=0x140000000
pe=pefile.PE(EXE,fast_load=True); data=open(EXE,"rb").read()
for s in pe.sections:
    if b".text" in s.Name: text=s;break
base=text.VirtualAddress; raw=text.PointerToRawData; size=text.SizeOfRawData
b=data[raw:raw+size]
dec=Decoder(64,b,ip=IB+base); fmt=Formatter(FormatterSyntax.INTEL)
writes=[]
for ins in dec:
    if (ins.op0_kind==OpKind.MEMORY and ins.memory_displacement==0xE8 and ins.memory_base not in (Register.NONE,Register.RSP,Register.RBP,Register.RIP)
        and ins.op1_kind==OpKind.REGISTER):
        # solo punteros (reg de 64 bits)
        # EN: pointers only (64-bit register) (not actually filtered)
        writes.append((ins.ip-IB, fmt.format(ins), ins.memory_base))
# marcadores: vtable RVAs como displacement RIP-relative, y offset 0x448
# EN: markers: vtable RVAs as RIP-relative displacement, and offset 0x448
VTS={0x16F10E8,0x16F1268}
# ES: Devuelve (usa +0x448, referencia vtable de animación) en la ventana alrededor de rva.
# EN: Returns (uses +0x448, references animation vtable) in the window around rva.
def ctx_flags(rva):
    # ventana hacia atras 0x600 hasta el prologo o limite
    # EN: backward window 0x600 up to the prologue or limit (code uses 0x700)
    lo=max(base, rva-0x700); hi=rva+0x40
    off=raw+(lo-base); length=hi-lo
    d=Decoder(64,data[off:off+length],ip=IB+lo)
    has448=False; hasvt=False
    for ins in d:
        if ins.memory_displacement==0x448 and ins.memory_base!=Register.NONE: has448=True
        # vtable cargada via lea rip-relative
        # EN: vtable loaded through rip-relative lea
        if ins.op1_kind==OpKind.MEMORY and ins.is_ip_rel_memory_operand:
            tgt=ins.ip_rel_memory_address-IB
            if tgt in VTS: hasvt=True
    return has448,hasvt
print("rva | write | has+0x448 | hasAnimVtable")
for rva,m,_ in writes:
    h448,hvt=ctx_flags(rva)
    mark=""
    if h448: mark+=" [+448]"
    if hvt: mark+=" [ANIMVT]"
    if h448 or hvt:
        print("0x%X: %-26s%s" % (rva,m,mark))
print("--- total writes ptr +0xE8: %d ---" % len(writes))
