exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
import re
# TypeDescriptor: vftable ptr (8) + spare (8) + name (mangled, ".?AV...@@\0")
# Buscamos la firma ".?AV" o ".?AU" en .data/.rdata
def scan_names():
    res=[]
    for sec in ['.data','.rdata']:
        srva,ssize=secs[sec]
        blob=bytes(data[srva:srva+ssize])
        for m in re.finditer(rb'\.\?A[VU][A-Za-z0-9_@?$]+@@', blob):
            name=m.group().decode('latin1')
            name_rva=srva+m.start()
            # El TypeDescriptor empieza 16 bytes antes (vftable+spare)
            td_rva=name_rva-16
            res.append((td_rva,name_rva,name,sec))
    return res
names=scan_names()
# Filtrar candidatos HUD/interface/menu/game
kw=['Hud','HUD','Interface','GameMenu','GameInterface','Paused','Pause','GameWindow','MainMenu','MenuStuff','GameUI','GUI']
print("=== Candidatos por keyword ===")
for td,nr,nm,sec in names:
    if any(k in nm for k in kw):
        print(f"  TD@{hex(td)} name@{hex(nr)} [{sec}] {nm}")
print("total typedescriptors:",len(names))
