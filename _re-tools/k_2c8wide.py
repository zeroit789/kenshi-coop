# ES: Serie "k_*" de investigación del HUD/panel de pausa. Este script recorre TODAS las funciones de
#     .pdata y lista las que LEEN [reg+0x2C8] (mov reg, [reg+0x2C8], reg distinto de pila) como
#     objeto, para ver quién usa el campo +0x2C8. Carga k_setup.py y k_regs.py con exec() desde la
#     ruta antigua C:/Users/Zero/ktmp (hay copias en _re-tools/). Uso: python k_2c8wide.py
# EN: "k_*" series investigating the HUD/pause panel. This script walks EVERY function in .pdata and
#     lists those that READ [reg+0x2C8] (mov reg, [reg+0x2C8], non-stack reg) as an object, to see
#     who uses field +0x2C8. Loads k_setup.py and k_regs.py via exec() from the old path
#     C:/Users/Zero/ktmp (copies live in _re-tools/). Usage: python k_2c8wide.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
exec(open(r"C:/Users/Zero/ktmp/k_regs.py").read())
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Register
fmt=Formatter(FormatterSyntax.INTEL)
OFF=0x2C8
STACK={Register.RBP,Register.RSP,Register.RIP}
# ES: Decodifica la función [b, e) completa.
# EN: Decodes the whole function [b, e).
def decode_func(b,e): return list(Decoder(64,data[b:e],ip=IB+b))

# Listar TODAS las funciones que LEEN [reg+0x2C8] (reg!=stack) como mov reg<-mem, sin exigir call.
# Para cada una reportamos contexto: que slots virtuales llama despues y si hay setVisible-like.
# EN: List ALL functions that READ [reg+0x2C8] (reg != stack) as mov reg<-mem, without requiring a call.
#     For each we report context: which virtual slots it calls afterwards and whether there is a
#     setVisible-like call (only the load sites are actually printed).
funcs={}
for (b,e,u) in PDATA:
    if not in_text(b): continue
    inss=decode_func(b,e)
    for i,ins in enumerate(inss):
        if ins.is_ip_rel_memory_operand: continue
        if ins.op_count>=2 and ins.op_kind(0)==OpKind.REGISTER and ins.op_kind(1)==OpKind.MEMORY \
           and ins.memory_displacement==OFF and ins.memory_base not in STACK and ins.memory_base!=Register.NONE \
           and ins.mnemonic==Mnemonic.MOV:
            funcs.setdefault(b,[]).append(ins.ip-IB)
print(f"funciones que LEEN [reg+0x2C8] como objeto: {len(funcs)}")
for fs in sorted(funcs):
    print(f"  {hex(fs)}: loads @ {[hex(x) for x in funcs[fs]]}")
