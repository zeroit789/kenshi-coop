import kdis, capstone, struct

IB = kdis.image_base
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

targets = {0x581770: 'process', 0x583400: 'create'}

text = None
for name, va, vsz, praw, rsz in kdis.sections:
    if name == '.text':
        text_va, text_sz, text_praw = va, max(vsz, rsz), praw
        text = kdis.data[text_praw:text_praw + text_sz]
        break

callers = {0x581770: [], 0x583400: []}
for i in range(len(text) - 5):
    if text[i] == 0xE8:
        rel = struct.unpack('<i', text[i + 1:i + 5])[0]
        src_rva = text_va + i
        dst_rva = src_rva + 5 + rel
        if dst_rva in targets:
            callers[dst_rva].append(src_rva)

for tgt, name in targets.items():
    print(f"\n### CALLERS de {name} (0x{tgt:X}): {len(callers[tgt])} sitios ###")
    for c in callers[tgt][:40]:
        print(f"  call @ 0x{c:06X}")
