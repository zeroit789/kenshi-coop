import kdis, capstone, struct

IB = kdis.image_base
targets = {0x581770: 'process', 0x583400: 'create'}

# Secciones
secs = {}
for name, va, vsz, praw, rsz in kdis.sections:
    secs[name] = (va, max(vsz, rsz), praw)

text_va, text_sz, text_praw = secs['.text']
text = kdis.data[text_praw:text_praw + text_sz]

# 1) Thunks JMP E9 rel32 -> target
thunks = {0x581770: [], 0x583400: []}
for i in range(len(text) - 5):
    if text[i] == 0xE9:
        rel = struct.unpack('<i', text[i + 1:i + 5])[0]
        src_rva = text_va + i
        dst_rva = src_rva + 5 + rel
        if dst_rva in targets:
            thunks[dst_rva].append(src_rva)

print("=== THUNKS (jmp E9) ===")
for tgt, name in targets.items():
    print(f"  {name} 0x{tgt:X}: thunks = {[hex(t) for t in thunks[tgt]]}")

# 2) Referencias absolutas qword (en .rdata = vtables) a target o a sus thunks
allvals = set(targets.keys())
for tgt in targets:
    for t in thunks[tgt]:
        allvals.add(t)
abs_targets = {IB + v: v for v in allvals}  # VA absoluta -> RVA

def scan_qword_refs(secname):
    va, sz, praw = secs[secname]
    blob = kdis.data[praw:praw + sz]
    hits = []
    for i in range(0, len(blob) - 8, 8):
        q = struct.unpack('<Q', blob[i:i + 8])[0]
        if q in abs_targets:
            hits.append((va + i, abs_targets[q]))
    return hits

for sname in ('.rdata', '.data'):
    print(f"\n=== qword refs (VA) en {sname} a process/create/thunks ===")
    for ref_rva, tgt_rva in scan_qword_refs(sname):
        print(f"  {sname} 0x{ref_rva:06X} -> 0x{tgt_rva:X}")

# 3) lea/mov rip-relative en .text a target o thunks (E8 ya descartado, buscamos call/jmp indirecto via puntero)
# Buscar lea reg,[rip+x] que apunte a un thunk (poco comun) - cubierto por refs absolutas de vtable.
