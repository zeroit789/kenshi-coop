# ES: Lee .text entero con ke_re y busca a mano las escrituras "mov [reg+0xE8], reg" sin SIB (REX.W 89,
#     mod=10, disp32 = E8 00 00 00). Imprime el total y cada RVA (origen de la lista de 106 de ke_task*).
#     Uso: python scan_e8.py
# EN: Reads the whole .text through ke_re and manually looks for "mov [reg+0xE8], reg" writes without SIB
#     (REX.W 89, mod=10, disp32 = E8 00 00 00). Prints the total and each RVA (likely the source of the
#     106-entry list used by ke_task*). Usage: python scan_e8.py

import ke_re as k

# Scan custom: mov [reg+0xE8], reg SIN SIB
# REX en {0x48,0x4C,0x49,0x4D}, opcode 0x89, modrm con mod=0b10, rm != 0b100 (SIB) y rm != 0b101,
# seguido de disp32 = 0xE8 0x00 0x00 0x00 (little-endian)
# Necesito acceso a los bytes de .text. Uso bytes_at_rva sobre toda la seccion en chunks.
# EN: Custom scan: mov [reg+0xE8], reg WITHOUT SIB
#     REX in {0x48,0x4C,0x49,0x4D}, opcode 0x89, modrm with mod=0b10, rm != 0b100 (SIB) and rm != 0b101,
#     followed by disp32 = 0xE8 0x00 0x00 0x00 (little-endian)
#     I need access to the .text bytes. I use bytes_at_rva over the whole section in chunks.

# Layout .text: VA 0x1000, size 0x1671412
# EN: .text layout: VA 0x1000, size 0x1671412
TEXT_START = 0x1000
TEXT_SIZE  = 0x1671412
TEXT_END   = TEXT_START + TEXT_SIZE

REX = {0x48,0x4C,0x49,0x4D}
results = []

CHUNK = 0x100000
rva = TEXT_START
prev_tail = b""
base_off = rva
buf = b""
# Leer todo en chunks y concatenar manejando solapamiento de 7 bytes
# EN: Read everything in chunks and concatenate, handling a 7-byte overlap (no overlap is actually needed)
data = bytearray()
pos = TEXT_START
while pos < TEXT_END:
    n = min(CHUNK, TEXT_END - pos)
    chunk = k.bytes_at_rva(pos, n)
    if chunk is None:
        break
    data += bytes(chunk)
    pos += n

print("len leido:", hex(len(data)))

# disp32 para +0xE8 = E8 00 00 00
# EN: disp32 for +0xE8 = E8 00 00 00
for i in range(len(data) - 7):
    if data[i] in REX and data[i+1] == 0x89:
        modrm = data[i+2]
        mod = (modrm >> 6) & 0b11
        reg = (modrm >> 3) & 0b111
        rm  = modrm & 0b111
        if mod == 0b10 and rm != 0b100 and rm != 0b101:
            # disp32 en i+3..i+6
            # EN: disp32 at i+3..i+6
            if data[i+3]==0xE8 and data[i+4]==0x00 and data[i+5]==0x00 and data[i+6]==0x00:
                rva_hit = TEXT_START + i
                results.append(rva_hit)

print("TOTAL escrituras mov [reg+0xE8],reg (sin SIB):", len(results))
for r in results:
    print(hex(r))
