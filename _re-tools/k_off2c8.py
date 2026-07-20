exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)

# Recorrer .text por funciones (.pdata) y dentro de cada funcion decodificar,
# buscando memoria con displacement 0x2C8 (base registro, no rip-rel).
# Recogemos: funcion, rva, si es escritura/lectura, y si cerca hay call [rax+0x18].
text=data
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
