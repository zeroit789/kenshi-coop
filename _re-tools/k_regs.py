# ES: Helper que se carga con exec() desde otros scripts k_*: construye REGNAME (valor -> nombre de
#     los registros de iced_x86) y rn(r) para imprimir nombres de registro.
# EN: Helper loaded via exec() from other k_* scripts: builds REGNAME (value -> name for iced_x86
#     registers) and rn(r) to print register names.

from iced_x86 import Register
REGNAME={}
for nm in dir(Register):
    if nm.startswith('_'): continue
    v=getattr(Register,nm)
    if isinstance(v,int): REGNAME[v]=nm
def rn(r): return REGNAME.get(r,f"r{r}")
