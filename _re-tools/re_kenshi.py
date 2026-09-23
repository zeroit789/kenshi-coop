# ES: Helper mínimo (lo importan find_refs.py, find_xrefs.py, pdata.py con "from re_kenshi import *"):
#     carga kenshi_x64.exe como imagen mapeada (data indexado por RVA) y ofrece disasm() y list_calls().
# EN: Minimal helper (imported by find_refs.py, find_xrefs.py, pdata.py via "from re_kenshi import *"):
#     loads kenshi_x64.exe as a mapped image (data indexed by RVA) and offers disasm() and list_calls().

import pefile, iced_x86
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind

PATH = r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
IMAGEBASE = 0x140000000

pe = pefile.PE(PATH, fast_load=True)
data = pe.get_memory_mapped_image()

# ES: Desensambla count instrucciones desde rva; imprime cabecera si hay label y devuelve la lista.
# EN: Disassembles count instructions from rva; prints a header if label is given and returns the list.
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

# ES: Lista los call directos dentro de [rva_start, rva_end) y los imprime.
# EN: Lists the direct calls inside [rva_start, rva_end) and prints them.
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
