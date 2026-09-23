# -*- coding: utf-8 -*-
# ES: Escaneo lineal de .text en 0x60C000-0x670000 (zona CharBody/CombatClass) listando escrituras
#     "mov [reg+0x68], reg" y "mov [reg+0x70], *" (flag amIdle). Nota: el comentario original habla
#     de 0x5C0000, pero el código usa LO=0x60C000. Uso: python re_cb68.py
# EN: Linear scan of .text in 0x60C000-0x670000 (CharBody/CombatClass area) listing
#     "mov [reg+0x68], reg" and "mov [reg+0x70], *" (amIdle flag) writes. Note: the original comment
#     says 0x5C0000, but the code uses LO=0x60C000. Usage: python re_cb68.py

# Busca escrituras 'mov [reg+0x68], reg' en .text en el rango CharBody/CombatClass (0x5C0000-0x670000)
# y lecturas 'cmp [reg+0x70],0' (amIdle). Reduce ruido restringiendo el rango.
# EN: Finds 'mov [reg+0x68], reg' writes in .text within the CharBody/CombatClass range (0x5C0000-0x670000)
#     and 'cmp [reg+0x70],0' reads (amIdle). Reduces noise by restricting the range.
#     (The code collects mov writes to +0x70, not cmp reads.)
import struct
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
EXE=r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"; IB=0x140000000
DATA=open(EXE,"rb").read()
# ES: Tabla de secciones desde las cabeceras PE.
# EN: Section table from the PE headers.
e=struct.unpack_from("<I",DATA,0x3C)[0]; coff=e+4
ns=struct.unpack_from("<H",DATA,coff+2)[0]; osz=struct.unpack_from("<H",DATA,coff+16)[0]
so=coff+20+osz; SEC=[]
for i in range(ns):
    o=so+i*40; nm=DATA[o:o+8].rstrip(b"\x00").decode("ascii","ignore")
    vs=struct.unpack_from("<I",DATA,o+8)[0]; rv=struct.unpack_from("<I",DATA,o+12)[0]
    rs=struct.unpack_from("<I",DATA,o+16)[0]; ro=struct.unpack_from("<I",DATA,o+20)[0]
    SEC.append((nm,rv,vs,ro,rs))
# ES: RVA -> offset de fichero.
# EN: RVA -> file offset.
def r2o(r):
    for nm,sr,vs,ro,rs in SEC:
        if sr<=r<sr+max(vs,rs) and r-sr<rs: return ro+(r-sr)
    return None
fmt=Formatter(FormatterSyntax.INTEL); fmt.hex_prefix="0x"; fmt.hex_suffix=""
# ES: Formateador Intel y mapa valor -> nombre de registro.
# EN: Intel formatter and value -> register name map.
RN={getattr(Register,n):n for n in dir(Register) if isinstance(getattr(Register,n),int) and not n.startswith("_")}
# Desensamblar funcion por funcion no es trivial; escaneo lineal del rango y reportar mov [reg+0x68],reg64 y mov byte[reg+0x70],imm
# EN: Disassembling function by function is not trivial; linear scan of the range reporting mov [reg+0x68],reg64 and mov byte[reg+0x70],imm
LO,HI=0x60C000,0x670000
o0=r2o(LO); code=DATA[o0:o0+(HI-LO)]
w68=[]; w70=[]
for ins in Decoder(64,code,ip=IB+LO):
    rr=ins.ip-IB
    if rr>HI: break
    if ins.mnemonic==Mnemonic.MOV and ins.op0_kind==OpKind.MEMORY and ins.memory_displacement==0x68 and ins.op1_kind==OpKind.REGISTER:
        w68.append((rr, RN.get(ins.memory_base,'?'), fmt.format(ins)))
    if ins.op0_kind==OpKind.MEMORY and ins.memory_displacement==0x70 and ins.mnemonic in (Mnemonic.MOV,) :
        w70.append((rr, RN.get(ins.memory_base,'?'), fmt.format(ins)))
print(f"=== mov [reg+0x68],reg64 en 0x{LO:X}-0x{HI:X}: {len(w68)} ===")
for rr,b,t in w68[:40]: print(f"  0x{rr:08X} base={b:<5} {t}")
print(f"\n=== mov [reg+0x70],* en rango: {len(w70)} ===")
for rr,b,t in w70[:40]: print(f"  0x{rr:08X} base={b:<5} {t}")
