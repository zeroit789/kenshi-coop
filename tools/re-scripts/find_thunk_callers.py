import kdis, capstone, struct

IB = kdis.image_base
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

# thunks descubiertos
thunk_process = 0x29195
thunk_create = 0x2C5A2
targets = {thunk_process: 'process_thunk', thunk_create: 'create_thunk'}

secs = {}
for name, va, vsz, praw, rsz in kdis.sections:
    secs[name] = (va, max(vsz, rsz), praw)
text_va, text_sz, text_praw = secs['.text']
text = kdis.data[text_praw:text_praw + text_sz]

callers = {thunk_process: [], thunk_create: []}
for i in range(len(text) - 5):
    if text[i] == 0xE8:
        rel = struct.unpack('<i', text[i + 1:i + 5])[0]
        src_rva = text_va + i
        dst_rva = src_rva + 5 + rel
        if dst_rva in targets:
            callers[dst_rva].append(src_rva)

def ctx(rva, back=64, fwd=8):
    """Desensambla `back` bytes antes hasta `fwd` instr despues del call."""
    start = rva - back
    off = kdis.rva_to_off(start)
    code = kdis.data[off:off + back + 24]
    lines = []
    for insn in md.disasm(code, IB + start):
        marker = ' <== CALL' if (insn.address - IB) == rva else ''
        b = ' '.join(f'{x:02X}' for x in insn.bytes)
        lines.append(f"  0x{insn.address-IB:06X}: {b:<24} {insn.mnemonic} {insn.op_str}{marker}")
        if (insn.address - IB) >= rva:
            break
    return lines

for tgt, name in targets.items():
    print(f"\n### CALLERS de {name} (0x{tgt:X}): {len(callers[tgt])} ###")
    for c in callers[tgt]:
        print(f"\n-- call site 0x{c:06X} --")
        for l in ctx(c):
            print(l)
