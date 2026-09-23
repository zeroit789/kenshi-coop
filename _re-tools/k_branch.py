# ES: Busca todas las ramas (call/jmp directos) al thunk 0x129A4 y a la función 0x72D3B0
#     (constructor investigado) con barrido lineal de .text, y además recorre .reloc buscando
#     entradas DIR64 cuyo valor sea la dirección de esa función o del thunk (punteros en tablas).
#     Carga k_setup.py desde C:/Users/Zero/ktmp. Uso: python k_branch.py
# EN: Finds every branch (direct call/jmp) to thunk 0x129A4 and function 0x72D3B0 (constructor under
#     investigation) with a linear sweep of .text, and also walks .reloc looking for DIR64 entries
#     whose value is the address of that function or thunk (pointers in tables).
#     Loads k_setup.py from C:/Users/Zero/ktmp. Usage: python k_branch.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
from iced_x86 import Decoder, OpKind, Mnemonic

# Buscar branches a thunk 0x129a4 y a func 0x72D3B0 con barrido lineal (puede tener falsos por desync,
# pero captura mas). Reportamos contexto de funcion.
# EN: Find branches to thunk 0x129a4 and function 0x72D3B0 with a linear sweep (may give false hits
#     from desync, but catches more). We report the containing function.
targets = {IB+0x129a4:'thunk', IB+0x72D3B0:'func'}
text_bytes = data[TEXT_RVA:TEXT_RVA+TEXT_SZ]
dec=Decoder(64,text_bytes,ip=IB+TEXT_RVA)
res=[]
for ins in dec:
    if ins.mnemonic in (Mnemonic.CALL,Mnemonic.JMP) and ins.op_count==1 and ins.op_kind(0)==OpKind.NEAR_BRANCH64:
        t=ins.near_branch_target
        if t in targets:
            res.append((ins.ip-IB,str(ins.mnemonic),targets[t]))
print("branches encontrados:")
for rva,mn,lab in res:
    fc=func_containing(rva)
    print(f"  {hex(rva)} {mn} -> {lab}  func={hex(fc[0]) if fc else '?'}")

# Ademas: buscar en .reloc relocaciones que apunten a la VA de la funcion (DIR64 entries).
# .reloc tiene bloques: header (PageRVA u32, BlockSize u32) + entries u16 (type<<12 | offset)
# EN: Also: search .reloc for relocations pointing to the function VA (DIR64 entries).
#     .reloc has blocks: header (PageRVA u32, BlockSize u32) + u16 entries (type<<12 | offset)
RELOC_RVA, RELOC_SZ = secs['.reloc']
off=RELOC_RVA; relhits=[]
fva=IB+0x72D3B0; thva=IB+0x129a4
end=RELOC_RVA+RELOC_SZ
while off < end-8:
    page=u32(off); blocksize=u32(off+4)
    if blocksize<8 or blocksize>0x10000: break
    nent=(blocksize-8)//2
    for i in range(nent):
        e=struct.unpack_from("<H",data,off+8+i*2)[0]
        typ=e>>12; o=e&0xFFF
        if typ==10:  # IMAGE_REL_BASED_DIR64
            loc=page+o
            if loc+8<=len(data):
                v=u64(loc)
                if v==fva or v==thva:
                    relhits.append((loc,v-IB))
    off+=blocksize
# ES: Informe de las relocaciones encontradas y la sección donde caen.
# EN: Report of the relocations found and the section they fall into.
print("reloc DIR64 apuntando a func/thunk:")
for loc,t in relhits[:50]:
    insec='.rdata' if in_rdata(loc) else ('.data' if in_data(loc) else ('.text' if in_text(loc) else '?'))
    print(f"   loc {hex(loc)} ({insec}) -> {hex(t)}")
print("total reloc hits:",len(relhits))
