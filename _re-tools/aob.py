import importlib.util, re
spec = importlib.util.spec_from_file_location("kdis", r"C:\Users\Zero\kdis.py")
k = importlib.util.module_from_spec(spec); spec.loader.exec_module(k)
# .text bounds
tname,tva,tvsz,tpraw,trsz = [s for s in k.sections if s[0]=='.text'][0]
text = k.data[tpraw:tpraw+trsz]
def count(pat):
    parts = pat.split()
    rx = b''.join((b'.' if p=='??' else re.escape(bytes([int(p,16)]))) for p in parts)
    return len(re.findall(rx, text, re.DOTALL))
pats = {
 "0x6B2630 (isAlly enriquecida)": "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 48 85 D2 75 0D 32 C0",
 "0x6744A0 (encolador)": "48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 41 8B E8 48 8B F2 48 8B F9 41 83 F8 04",
 "0x7923D0 (Character::isAlly)": "48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 41 0F B6 E8 48 8B FA 48 8B F1 48 85 D2 75 12",
 "0x6B26D0 (isEnemy rel<=-30)": "40 53 48 83 EC 20 48 8B D9 48 85 D2 75 08 32 C0 48 83 C4 20 5B C3 48 3B 51 08 74 2F",
}
for n,p in pats.items():
    print(f"  matches={count(p):<3} {n}")
