# ES: Recorre todas las funciones de .pdata y lista cada acceso a memoria [reg+0x2C8] (no RIP-relativo),
#     agrupado por función. Base de la búsqueda del campo +0x2C8 del HUD.
#     Carga k_setup.py desde C:/Users/Zero/ktmp. Uso: python k_off2c8.py
# EN: Walks every .pdata function and lists each [reg+0x2C8] memory access (not RIP-relative),
#     grouped by function. Starting point for the HUD +0x2C8 field search.
#     Loads k_setup.py from C:/Users/Zero/ktmp. Usage: python k_off2c8.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)

# Recorrer .text por funciones (.pdata) y dentro de cada funcion decodificar,
# buscando memoria con displacement 0x2C8 (base registro, no rip-rel).
# Recogemos: funcion, rva, si es escritura/lectura, y si cerca hay call [rax+0x18].
# EN: Walk .text per function (.pdata) and decode each function,
#     looking for memory operands with displacement 0x2C8 (register base, not rip-rel).
#     We collect: function, rva, read/write, and whether there is a call [rax+0x18] nearby
#     (only function, rva and text are actually collected).
text=data
# ES: Decodifica la función [b_rva, e_rva) completa.
# EN: Decodes the whole function [b_rva, e_rva).
def decode_func(b_rva,e_rva):
    b=data[b_rva:e_rva]
    dec=Decoder(64,b,ip=IB+b_rva)
    return list(dec)

OFF=0x2C8
results={}  # func_start -> list of (rva, txt, mem_disp)
for (b,e,u) in PDATA:
    if not in_text(b): continue
    inss=decode_func(b,e)
    found=[]
    for idx,ins in enumerate(inss):
        if ins.is_ip_rel_memory_operand: 
            continue
        # revisar operandos memoria
        # EN: check memory operands
        for opi in range(ins.op_count):
            if ins.op_kind(opi)==OpKind.MEMORY:
                if ins.memory_displacement==OFF and ins.memory_base!=Register.NONE and ins.memory_base!=Register.RIP:
                    found.append((ins.ip-IB, fmt.format(ins)))
    if found:
        results[b]=found

print(f"funciones que tocan [reg+0x2C8]: {len(results)}")
for fstart,lst in sorted(results.items()):
    print(f"\n--- func {hex(fstart)} ({len(lst)} accesos) ---")
    for rva,txt in lst:
        print(f"   {hex(rva)}: {txt}")
