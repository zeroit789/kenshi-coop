import pefile, capstone, sys

PE_PATH = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
pe = pefile.PE(PE_PATH, fast_load=True)
image_base = pe.OPTIONAL_HEADER.ImageBase
sections = []
for s in pe.sections:
    name = s.Name.rstrip(b'\x00').decode('latin1')
    sections.append((name, s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData))

def rva_to_off(rva):
    for name,va,vsz,praw,rsz in sections:
        if va <= rva < va+max(vsz,rsz):
            return praw + (rva-va)
    return None

with open(PE_PATH,'rb') as f:
    data = f.read()

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

def read_rva(rva, n):
    off = rva_to_off(rva)
    return data[off:off+n]

def disasm(rva, length=400, stop_on_ret=True):
    off = rva_to_off(rva)
    code = data[off:off+length]
    out=[]
    count=0
    for insn in md.disasm(code, image_base+rva):
        out.append(insn)
        count+=1
        if stop_on_ret and insn.mnemonic in ('ret','jmp') and count>3:
            # heuristic: stop at first ret; for jmp only if it's likely a tail
            if insn.mnemonic=='ret':
                break
    return out

def show(rva, length=400, label=""):
    print(f"\n===== {label} @ RVA 0x{rva:X}  (VA 0x{image_base+rva:X}) =====")
    for insn in disasm(rva, length):
        b = ' '.join(f'{x:02X}' for x in insn.bytes)
        print(f"0x{insn.address-image_base:06X}: {b:<28} {insn.mnemonic} {insn.op_str}")

if __name__=='__main__':
    print("ImageBase=0x%X" % image_base)
    for n,va,vsz,praw,rsz in sections:
        print(f"  {n:8} RVA 0x{va:07X} vsz 0x{vsz:X} raw 0x{praw:X}")
