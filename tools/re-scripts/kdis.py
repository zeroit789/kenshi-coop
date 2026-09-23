# ES: Helper común de re-scripts (import kdis): abre kenshi_x64.exe con pefile, guarda la lista de
#     secciones, lee el fichero completo en data y ofrece rva_to_off, read_rva, disasm y show con Capstone
#     (md). Ejecutado directamente imprime la base de imagen y las secciones. Uso: python kdis.py
# EN: Common re-scripts helper (import kdis): opens kenshi_x64.exe with pefile, stores the section list,
#     reads the whole file into data and offers rva_to_off, read_rva, disasm and show with Capstone (md).
#     Run directly it prints the image base and the sections. Usage: python kdis.py

import pefile, capstone, sys

PE_PATH = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
pe = pefile.PE(PE_PATH, fast_load=True)
image_base = pe.OPTIONAL_HEADER.ImageBase
sections = []
for s in pe.sections:
    name = s.Name.rstrip(b'\x00').decode('latin1')
    sections.append((name, s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData))

# ES: RVA -> offset de fichero según la tabla de secciones.
# EN: RVA -> file offset from the section table.
def rva_to_off(rva):
    for name,va,vsz,praw,rsz in sections:
        if va <= rva < va+max(vsz,rsz):
            return praw + (rva-va)
    return None

with open(PE_PATH,'rb') as f:
    data = f.read()

# ES: Desensamblador Capstone x86-64 con detalle de operandos.
# EN: Capstone x86-64 disassembler with operand details.
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

# ES: n bytes a partir de un RVA.
# EN: n bytes starting at an RVA.
def read_rva(rva, n):
    off = rva_to_off(rva)
    return data[off:off+n]

# ES: Lista de instrucciones Capstone desde rva; con stop_on_ret para en el primer ret (tras 3 instrucciones).
# EN: List of Capstone instructions from rva; with stop_on_ret it stops at the first ret (after 3 instructions).
def disasm(rva, length=400, stop_on_ret=True):
    off = rva_to_off(rva)
    code = data[off:off+length]
    out=[]
    count=0
    for insn in md.disasm(code, image_base+rva):
        out.append(insn)
        count+=1
        if stop_on_ret and insn.mnemonic in ('ret','jmp') and count>3:
            # heuristic: stop at first ret; for jmp only if it's likely a tail
            if insn.mnemonic=='ret':
                break
    return out

# ES: Imprime el desensamblado con bytes y una cabecera con etiqueta.
# EN: Prints the disassembly with bytes and a labeled header.
def show(rva, length=400, label=""):
    print(f"\n===== {label} @ RVA 0x{rva:X}  (VA 0x{image_base+rva:X}) =====")
    for insn in disasm(rva, length):
        b = ' '.join(f'{x:02X}' for x in insn.bytes)
        print(f"0x{insn.address-image_base:06X}: {b:<28} {insn.mnemonic} {insn.op_str}")

# ES: Ejecución directa: base de imagen y secciones.
# EN: Direct run: image base and sections.
if __name__=='__main__':
    print("ImageBase=0x%X" % image_base)
    for n,va,vsz,praw,rsz in sections:
        print(f"  {n:8} RVA 0x{va:07X} vsz 0x{vsz:X} raw 0x{praw:X}")
