# ES: Busca qwords en .rdata/.data que apunten dentro de la función 0x72D3B0-0x72FC26, construye el
#     mapa de thunks JMP de 5 bytes de la zona 0x12000-0x14000 y busca en vtables los thunks que
#     saltan a 0x72D3B0. Carga k_setup.py desde C:/Users/Zero/ktmp. Uso: python k_vtab2.py
# EN: Searches .rdata/.data for qwords pointing inside function 0x72D3B0-0x72FC26, builds the map of
#     5-byte JMP thunks in the 0x12000-0x14000 area and looks in vtables for thunks jumping to
#     0x72D3B0. Loads k_setup.py from C:/Users/Zero/ktmp. Usage: python k_vtab2.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, OpKind, Mnemonic

FSTART=0x72D3B0; FEND=0x72FC26
lo=IB+FSTART; hi=IB+FEND

# Buscar cualquier qword en .rdata/.data que apunte DENTRO de la funcion (vtable a func real o a thunk)
# Tambien thunk range: los thunks viven ~0x12000-0x13000. Busquemos qwords que apunten a esa zona
# y luego veamos si ese thunk salta dentro de la funcion.
# EN: Find any qword in .rdata/.data pointing INSIDE the function (vtable to real func or to thunk)
#     Also the thunk range: thunks live around 0x12000-0x13000. Find qwords pointing there
#     and then check whether that thunk jumps inside the function.
for sec in ['.rdata','.data']:
    srva,ssize=secs[sec]
    a=(srva+7)&~7
    direct=[]
    for rva in range(a, srva+ssize-8,8):
        q=u64(rva)
        if lo<=q<hi:
            direct.append((rva,q-IB))
    if direct:
        print(f"[{sec}] qwords apuntando dentro de la funcion:")
        for rva,t in direct: print(f"   ptr@{hex(rva)} -> {hex(t)}")

# Ahora: enumerar TODOS los thunks JMP del bloque 0x12000-0x13500 y mapear thunk_va->dest
# EN: Now: enumerate ALL JMP thunks in the 0x12000-0x13500 block and map thunk_va->dest (code uses up to 0x14000)
thunk_lo=0x12000; thunk_hi=0x14000
tb=data[thunk_lo:thunk_hi]
dec=Decoder(64,tb,ip=IB+thunk_lo)
thunkmap={}  # dest_rva -> thunk_rva
for ins in dec:
    if ins.mnemonic==Mnemonic.JMP and ins.op_count==1 and ins.op_kind(0)==OpKind.NEAR_BRANCH64 and ins.len==5:
        dest=ins.near_branch_target-IB
        thunkmap[ins.ip-IB]=dest
# thunk para nuestra funcion
# EN: thunk(s) for our function
our_thunks=[t for t,d in thunkmap.items() if d==FSTART]
print("thunks que saltan a 0x72D3B0:", [hex(t) for t in our_thunks])

# para cada thunk, buscar su VA en vtables
# EN: for each thunk, look for its VA in vtables
for t in our_thunks:
    tva=IB+t
    for sec in ['.rdata','.data']:
        srva,ssize=secs[sec]
        a=(srva+7)&~7
        for rva in range(a,srva+ssize-8,8):
            if u64(rva)==tva:
                print(f"  thunk {hex(t)} en vtable {sec}@{hex(rva)}")
