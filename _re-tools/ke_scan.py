# busca instrucciones que escriban/lean [reg+0x448] con patron de mov. Reporta RVA en .text - READ ONLY
import sys, ke_dis
ke_dis.load(); img=ke_dis._image
# patron: mov [reg+disp32]=... o lea: buscamos disp 48 04 00 00 (0x448 LE) en .text
needle=bytes([0x48,0x04,0x00,0x00])  # 0x00000448 LE
i=img.find(needle, 0x1000)
hits=[]
cnt=0
while i!=-1 and i<0x1672000 and cnt<60:
    # contexto 8 bytes antes
    ctx=img[i-3:i+4]
    hits.append((i, " ".join(f"{b:02X}" for b in ctx)))
    cnt+=1
    i=img.find(needle, i+1)
for h in hits:
    print(f"0x{h[0]-3:08X}: ...{h[1]}")
