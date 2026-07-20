from iced_x86 import Register
REGNAME={}
for nm in dir(Register):
    if nm.startswith('_'): continue
    v=getattr(Register,nm)
    if isinstance(v,int): REGNAME[v]=nm
def rn(r): return REGNAME.get(r,f"r{r}")
