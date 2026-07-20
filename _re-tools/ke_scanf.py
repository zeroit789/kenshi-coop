# busca disp 0x448 dentro de un rango [start,end)
import sys, ke_dis
ke_dis.load(); img=ke_dis._image
start=int(sys.argv[1],16); end=int(sys.argv[2],16); disp=int(sys.argv[3],16)
needle=disp.to_bytes(4,'little')
i=img.find(needle,start)
while i!=-1 and i<end:
    ctx=img[i-3:i+4]
    print(f"0x{i-3:08X}: {' '.join(f'{b:02X}' for b in ctx)}")
    i=img.find(needle,i+1)
