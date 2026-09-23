# ES: Busca en .rdata/.data qwords con la dirección absoluta del thunk 0x129A4 y de la función
#     0x72D3B0 (posibles entradas de vtable). Carga k_setup.py desde C:/Users/Zero/ktmp.
#     Uso: python k_vtab.py
# EN: Searches .rdata/.data for qwords holding the absolute address of thunk 0x129A4 and of function
#     0x72D3B0 (possible vtable entries). Loads k_setup.py from C:/Users/Zero/ktmp.
#     Usage: python k_vtab.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())

thunk_rva = 0x129a4
thunk_va = IB + thunk_rva

# ES: RVAs alineados a 8 de la sección donde hay un qword igual a va.
# EN: 8-aligned RVAs in the section holding a qword equal to va.
def find_qword(va, secname):
    srva, ssize = secs[secname]
    a=(srva+7)&~7
    hits=[]
    for rva in range(a, srva+ssize-8, 8):
        if u64(rva)==va: hits.append(rva)
    return hits

for sec in ['.rdata','.data']:
    h = find_qword(thunk_va, sec)
    print(f"VA thunk 0x{thunk_va:x} encontrado en {sec}:", [hex(x) for x in h])

# Tambien la VA real de la funcion (por si alguna vtable usa la directa)
# EN: Also the real VA of the function (in case some vtable uses it directly)
fva = IB+0x72D3B0
for sec in ['.rdata','.data']:
    h = find_qword(fva, sec)
    if h: print(f"VA func directa 0x{fva:x} en {sec}:", [hex(x) for x in h])
