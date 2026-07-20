exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())
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
for r in [0x170AA48,0x170AA38,0x170AA30]:
    print(hex(r), "->", repr(readstr(r)))
