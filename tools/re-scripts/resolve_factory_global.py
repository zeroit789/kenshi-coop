# ES: Para cada llamada a los thunks de las factories (process 0x29195 / create 0x2C5A2), busca hacia
#     atrás la última instrucción que escribe rcx; si es una carga RIP-relativa, resuelve la variable
#     global usada como "this" y cuenta su frecuencia (con su distancia a GameWorld 0x2134110).
#     Usa kdis.py + Capstone. Uso: python resolve_factory_global.py
# EN: For each call to the factory thunks (process 0x29195 / create 0x2C5A2), searches backwards for the
#     last instruction writing rcx; if it is a RIP-relative load, resolves the global variable used as
#     "this" and counts its frequency (with its distance to GameWorld 0x2134110).
#     Uses kdis.py + Capstone. Usage: python resolve_factory_global.py

import kdis, capstone, struct
from collections import Counter

IB = kdis.image_base
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

thunk_process = 0x29195
thunk_create = 0x2C5A2
targets = {thunk_process: 'process', thunk_create: 'create'}

secs = {}
for name, va, vsz, praw, rsz in kdis.sections:
    secs[name] = (va, max(vsz, rsz), praw)
text_va, text_sz, text_praw = secs['.text']
text = kdis.data[text_praw:text_praw + text_sz]

# ES: Llamadas directas a cada thunk.
# EN: Direct calls to each thunk.
callers = {thunk_process: [], thunk_create: []}
for i in range(len(text) - 5):
    if text[i] == 0xE8:
        rel = struct.unpack('<i', text[i + 1:i + 5])[0]
        src_rva = text_va + i
        dst_rva = src_rva + 5 + rel
        if dst_rva in targets:
            callers[dst_rva].append(src_rva)

# ES: Nombre de la sección que contiene el RVA.
# EN: Name of the section containing the RVA.
def sec_of(rva):
    for n, (va, sz, praw) in secs.items():
        if va <= rva < va + sz:
            return n
    return '?'

# ES: Última instrucción antes del call que escribe rcx: (descripción, RVA global si es RIP-relativa).
# EN: Last instruction before the call that writes rcx: (description, global RVA if RIP-relative).
def find_rcx_source(call_rva):
    """Desensambla hacia atras desde el call y encuentra la ultima instr que
    escribe rcx. Devuelve (desc, rip_global_rva or None)."""
    start = call_rva - 80
    off = kdis.rva_to_off(start)
    code = kdis.data[off:off + 80 + 8]
    last_rcx = None
    for insn in md.disasm(code, IB + start):
        if (insn.address - IB) > call_rva:
            break
        # detectar destino rcx
        # EN: detect rcx as destination
        ops = insn.op_str
        if insn.mnemonic in ('mov', 'lea') and ops.startswith('rcx,'):
            rip_rva = None
            # rip-relative: mov rcx, qword ptr [rip + X]
            # EN: rip-relative: mov rcx, qword ptr [rip + X]
            if 'rip' in ops:
                # disp esta en insn; recompute desde bytes
                # capstone da la VA destino directamente en operand
                # EN: disp is in insn; recompute from bytes
                #     capstone gives the target VA directly in the operand
                for op in insn.operands:
                    if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                        ptr_va = insn.address + insn.size + op.mem.disp
                        rip_rva = ptr_va - IB
            last_rcx = (f"0x{insn.address-IB:06X}: {insn.mnemonic} {ops}", rip_rva)
    return last_rcx

# ES: Informe por thunk y frecuencia de globales.
# EN: Per-thunk report and global frequency.
globals_ctr = Counter()
for tgt, name in targets.items():
    print(f"\n### {name} (0x{tgt:X}) — {len(callers[tgt])} callers — fuente de rcx ###")
    for c in callers[tgt]:
        res = find_rcx_source(c)
        if res:
            desc, rip = res
            if rip is not None:
                ptr_val = struct.unpack('<Q', kdis.data[kdis.rva_to_off(rip):kdis.rva_to_off(rip)+8])[0]
                print(f"  call 0x{c:06X}: {desc}  -> GLOBAL @RVA 0x{rip:X} ({sec_of(rip)}) = qword 0x{ptr_val:X}")
                globals_ctr[rip] += 1
            else:
                print(f"  call 0x{c:06X}: {desc}  (rcx no rip-relative)")
        else:
            print(f"  call 0x{c:06X}: (no se hallo escritor de rcx)")

print("\n### GLOBALES rip-relative usados como rcx (factory) — frecuencia ###")
for rva, cnt in globals_ctr.most_common():
    print(f"  RVA 0x{rva:X} ({sec_of(rva)}): {cnt} sitios   offset-from-GW(0x2134110)=0x{rva-0x2134110:X}")
