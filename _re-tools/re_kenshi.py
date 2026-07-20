import pefile, iced_x86
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind

PATH = r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
IMAGEBASE = 0x140000000

pe = pefile.PE(PATH, fast_load=True)
data = pe.get_memory_mapped_image()

def disasm(rva, count=40, label=""):
    code = data[rva: rva + count*16]
    decoder = Decoder(64, code, ip=IMAGEBASE + rva)
    fmt = Formatter(FormatterSyntax.INTEL)
    fmt.hex_prefix = "0x"; fmt.hex_suffix = ""
    out = []
    n = 0
    base = IMAGEBASE + rva
    for instr in decoder:
        if n >= count: break
        abs_ip = instr.ip
        rva_ip = abs_ip - IMAGEBASE
        text = fmt.format(instr)
        start = abs_ip - base
        b = code[start:start+instr.len]
        bytestr = " ".join(f"{x:02X}" for x in b)
        out.append((rva_ip, abs_ip, text, bytestr, instr))
        n += 1
    if label:
        print(f"\n===== {label} (RVA 0x{rva:X} / abs 0x{base:X}) =====")
    for rva_ip, abs_ip, text, bytestr, instr in out:
        print(f"0x{rva_ip:X}  {text:<50} ; {bytestr}")
    return out

def list_calls(rva_start, rva_end, label=""):
    code = data[rva_start: rva_end]
    decoder = Decoder(64, code, ip=IMAGEBASE + rva_start)
    calls = []
    for instr in decoder:
        if instr.mnemonic == Mnemonic.CALL and instr.op_kind(0) == OpKind.NEAR_BRANCH64:
            tgt_rva = instr.near_branch_target - IMAGEBASE
            calls.append((instr.ip - IMAGEBASE, tgt_rva))
    if label:
        print(f"\n===== CALLs en {label} 0x{rva_start:X}-0x{rva_end:X} =====")
    for src, tgt in calls:
        print(f"  0x{src:X} -> call 0x{tgt:X}")
    return calls
