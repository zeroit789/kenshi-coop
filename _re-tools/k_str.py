# ES: Imprime las cadenas de .rdata en varios RVAs (0x16DE368, 0x16E127C, 0x17043B0) y las del panel
#     de pausa (PausedPanel/lbPaused/PAUSED). Carga k_setup.py desde C:/Users/Zero/ktmp.
#     Uso: python k_str.py
# EN: Prints the .rdata strings at several RVAs (0x16DE368, 0x16E127C, 0x17043B0) and the pause-panel
#     ones (PausedPanel/lbPaused/PAUSED). Loads k_setup.py from C:/Users/Zero/ktmp.
#     Usage: python k_str.py

exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
# ES: Lee una cadena terminada en 0 (latin1) desde un RVA.
# EN: Reads a NUL-terminated string (latin1) from an RVA.
def readstr(rva,maxlen=200):
    b=bytearray()
    i=rva
    while i<len(data) and data[i]!=0 and len(b)<maxlen:
        b.append(data[i]); i+=1
    try: return b.decode('latin1')
    except: return repr(bytes(b))
for r in [0x16de368,0x16e127c,0x17043b0]:
    print(hex(r), "->", repr(readstr(r)))
# Tambien los strings del HUD de pausa que ya conoce Zero
# EN: Also the pause HUD strings already known
for r in [0x170AA48,0x170AA38,0x170AA30]:
    print(hex(r), "->", repr(readstr(r)))
