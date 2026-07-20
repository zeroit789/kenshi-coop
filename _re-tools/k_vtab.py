exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())

thunk_rva = 0x129a4
thunk_va = IB + thunk_rva

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
fva = IB+0x72D3B0
for sec in ['.rdata','.data']:
    h = find_qword(fva, sec)
    if h: print(f"VA func directa 0x{fva:x} en {sec}:", [hex(x) for x in h])
