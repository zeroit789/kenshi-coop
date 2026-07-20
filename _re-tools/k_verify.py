exec(open(r"C:/Users/Zero/ktmp/k_setup.py").read())

def hexbytes(rva,n): return ' '.join(f'{data[rva+i]:02X}' for i in range(n))

print("=== 1. Bytes en 0x6E20D0 (updatePauseUI) ===")
print(hexbytes(0x6E20D0,37))
aob="40 53 48 83 EC 20 48 8B 49 10 0F B6 DA 48 85 C9 74 12 E8"
got=' '.join(f'{data[0x6E20D0+i]:02X}' for i in range(len(aob.split())))
print("match prefijo:", got==aob)

print("\n=== 2. Container 0x21337B0 en .data ===")
print("in_data:", in_data(0x21337B0))

print("\n=== 3. Bytes en 0x787D40 (setPaused) prefijo ===")
print(hexbytes(0x787D40,16))

print("\n=== 4. getter 0x720F50 (mov rax,[rcx+2C8]; ret) ===")
print(hexbytes(0x720F50,8))

# Verificar unicidad de AOB updatePauseUI en .text
import re
def count_aob(aob_str):
    toks=aob_str.split(); pat=bytearray(); mask=[]
    for t in toks:
        if t=='??': pat.append(0); mask.append(False)
        else: pat.append(int(t,16)); mask.append(True)
    tb=data[TEXT_RVA:TEXT_RVA+TEXT_SZ]
    n=len(pat); cnt=0; first=None
    i=0; L=len(tb)
    while i<=L-n:
        ok=True
        for j in range(n):
            if mask[j] and tb[i+j]!=pat[j]: ok=False; break
        if ok:
            cnt+=1
            if first is None: first=TEXT_RVA+i
        i+=1
    return cnt,first

aob_upd="40 53 48 83 EC 20 48 8B 49 10 0F B6 DA 48 85 C9 74 12 E8 ?? ?? ?? ?? 0F B6 D3 4C 8B 00 48 8B C8 41 FF 50 18"
c,f=count_aob(aob_upd)
print(f"\n=== 5. AOB updatePauseUI: {c} match(es), primero @ {hex(f) if f else None} ===")

aob_sp="48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 0F 29 74 24 40 F3 0F 10 B1 00 07 00 00 33 F6 0F 2E 35 ?? ?? ?? ?? 0F B6 DA 48 8B F9"
c2,f2=count_aob(aob_sp)
print(f"=== 6. AOB setPaused: {c2} match(es), primero @ {hex(f2) if f2 else None} ===")
