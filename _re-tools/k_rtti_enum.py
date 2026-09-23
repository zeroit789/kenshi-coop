# ES: Enumera todos los TypeDescriptor RTTI de .data/.rdata buscando nombres decorados ".?AV"/".?AU"
#     y muestra los que contienen palabras de HUD/interfaz/menú/pausa. Carga k_setup.py desde
#     C:/Users/Zero/ktmp. Uso: python k_rtti_enum.py
# EN: Enumerates every RTTI TypeDescriptor in .data/.rdata by searching mangled ".?AV"/".?AU" names
#     and shows those containing HUD/interface/menu/pause keywords. Loads k_setup.py from
#     C:/Users/Zero/ktmp. Usage: python k_rtti_enum.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
import re
# TypeDescriptor: vftable ptr (8) + spare (8) + name (mangled, ".?AV...@@\0")
# Buscamos la firma ".?AV" o ".?AU" en .data/.rdata
# EN: TypeDescriptor: vftable ptr (8) + spare (8) + name (mangled, ".?AV...@@\0")
#     We search for the ".?AV" or ".?AU" signature in .data/.rdata
# ES: Devuelve (TD, RVA del nombre, nombre, sección) de cada nombre RTTI encontrado.
# EN: Returns (TD, name RVA, name, section) for every RTTI name found.
def scan_names():
    res=[]
    for sec in ['.data','.rdata']:
        srva,ssize=secs[sec]
        blob=bytes(data[srva:srva+ssize])
        for m in re.finditer(rb'\.\?A[VU][A-Za-z0-9_@?$]+@@', blob):
            name=m.group().decode('latin1')
            name_rva=srva+m.start()
            # El TypeDescriptor empieza 16 bytes antes (vftable+spare)
            # EN: The TypeDescriptor starts 16 bytes earlier (vftable+spare)
            td_rva=name_rva-16
            res.append((td_rva,name_rva,name,sec))
    return res
names=scan_names()
# Filtrar candidatos HUD/interface/menu/game
# EN: Filter HUD/interface/menu/game candidates
kw=['Hud','HUD','Interface','GameMenu','GameInterface','Paused','Pause','GameWindow','MainMenu','MenuStuff','GameUI','GUI']
print("=== Candidatos por keyword ===")
for td,nr,nm,sec in names:
    if any(k in nm for k in kw):
        print(f"  TD@{hex(td)} name@{hex(nr)} [{sec}] {nm}")
print("total typedescriptors:",len(names))
