# ES: Busca un desplazamiento de 32 bits (little-endian) dentro de un rango de RVAs e imprime cada
#     coincidencia con 3 bytes de contexto previo. Depende de ke_dis.load()/ke_dis._image (no
#     definidos en el ke_dis.py versionado). Uso: python ke_scanf.py <start_hex> <end_hex> <disp_hex>
# EN: Searches a 32-bit displacement (little-endian) within an RVA range and prints each match with 3
#     bytes of preceding context. Depends on ke_dis.load()/ke_dis._image (not defined in the versioned
#     ke_dis.py). Usage: python ke_scanf.py <start_hex> <end_hex> <disp_hex>

# busca disp 0x448 dentro de un rango [start,end)
# EN: finds disp 0x448 (any disp given on the command line) inside a range [start,end)
import sys, ke_dis
ke_dis.load(); img=ke_dis._image
start=int(sys.argv[1],16); end=int(sys.argv[2],16); disp=int(sys.argv[3],16)
needle=disp.to_bytes(4,'little')
i=img.find(needle,start)
while i!=-1 and i<end:
    ctx=img[i-3:i+4]
    print(f"0x{i-3:08X}: {' '.join(f'{b:02X}' for b in ctx)}")
    i=img.find(needle,i+1)
