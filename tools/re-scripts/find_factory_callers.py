# ES: Serie re-scripts (usa el helper kdis.py de esta carpeta + Capstone). Lista los call E8 directos
#     a las funciones factory "process" (0x581770) y "create" (0x583400). Uso: python find_factory_callers.py
# EN: re-scripts series (uses this folder's kdis.py helper + Capstone). Lists the direct E8 calls to the
#     "process" (0x581770) and "create" (0x583400) factory functions. Usage: python find_factory_callers.py

import kdis, capstone, struct

IB = kdis.image_base
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

targets = {0x581770: 'process', 0x583400: 'create'}

# ES: Localizar .text en el fichero.
# EN: Locate .text in the file.
text = None
for name, va, vsz, praw, rsz in kdis.sections:
    if name == '.text':
        text_va, text_sz, text_praw = va, max(vsz, rsz), praw
        text = kdis.data[text_praw:text_praw + text_sz]
        break

# ES: Escaneo byte a byte de call rel32.
# EN: Byte-by-byte scan for rel32 calls.
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
