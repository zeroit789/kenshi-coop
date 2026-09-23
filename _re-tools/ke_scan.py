# ES: Busca en la imagen los bytes 48 04 00 00 (desplazamiento 0x448 little-endian) a partir de .text
#     y muestra hasta 60 coincidencias con contexto: candidatos a accesos [reg+0x448].
#     Depende de ke_dis.load()/ke_dis._image (no definidos en el ke_dis.py versionado).
#     Uso: python ke_scan.py
# EN: Searches the image for bytes 48 04 00 00 (displacement 0x448 little-endian) starting at .text
#     and shows up to 60 matches with context: [reg+0x448] access candidates.
#     Depends on ke_dis.load()/ke_dis._image (not defined in the versioned ke_dis.py).
#     Usage: python ke_scan.py

# busca instrucciones que escriban/lean [reg+0x448] con patron de mov. Reporta RVA en .text - READ ONLY
# EN: finds instructions that write/read [reg+0x448] with a mov pattern. Reports RVA in .text - READ ONLY
import sys, ke_dis
ke_dis.load(); img=ke_dis._image
# patron: mov [reg+disp32]=... o lea: buscamos disp 48 04 00 00 (0x448 LE) en .text
# EN: pattern: mov [reg+disp32]=... or lea: we look for disp 48 04 00 00 (0x448 LE) in .text
needle=bytes([0x48,0x04,0x00,0x00])  # 0x00000448 LE
i=img.find(needle, 0x1000)
hits=[]
cnt=0
while i!=-1 and i<0x1672000 and cnt<60:
    # contexto 8 bytes antes
    # EN: context: 3 bytes before and 4 from the match (the original comment says 8)
    ctx=img[i-3:i+4]
    hits.append((i, " ".join(f"{b:02X}" for b in ctx)))
    cnt+=1
    i=img.find(needle, i+1)
for h in hits:
    print(f"0x{h[0]-3:08X}: ...{h[1]}")
