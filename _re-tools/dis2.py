# ES: Navaja suiza de desensamblado de kenshi_x64.exe (pefile + iced_x86). Modos por línea de comandos:
#       python dis2.py calls <rva> [len]     -> llamadas directas/indirectas dentro de una función
#       python dis2.py d <rva> [bytes] [max] -> desensamblado con bytes
#       python dis2.py aob "<patrón>"        -> busca un patrón AOB en .text
#       python dis2.py callers <rva>         -> call E8 que apuntan al RVA
#       python dis2.py refs <rva>            -> búsqueda burda del dword del RVA en .text
#     find_disp() queda definida pero no se usa desde la línea de comandos.
# EN: Disassembly swiss-army knife for kenshi_x64.exe (pefile + iced_x86). Command-line modes:
#       python dis2.py calls <rva> [len]     -> direct/indirect calls inside a function
#       python dis2.py d <rva> [bytes] [max] -> disassembly with bytes
#       python dis2.py aob "<pattern>"       -> search an AOB pattern in .text
#       python dis2.py callers <rva>         -> E8 calls targeting the RVA
#       python dis2.py refs <rva>            -> crude search for the RVA dword in .text
#     find_disp() is defined but not reachable from the command line.

import sys, pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind
PATH = r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe"
IB=0x140000000
pe=pefile.PE(PATH, fast_load=True)
secs=[(s.Name.rstrip(b'\x00').decode('latin1'),s.VirtualAddress,s.Misc_VirtualSize) for s in pe.sections]
# ES: Lista (sitio, destino) de las llamadas de la función en rva; las indirectas se devuelven formateadas.
# EN: Lists (site, target) for the calls in the function at rva; indirect ones are returned formatted.
def calls(rva,length=0x900,stop_pad=True):
    data=pe.get_data(rva,length); dec=Decoder(64,data,ip=IB+rva); out=[]
    prev_ret=False
    for ins in dec:
        off=ins.ip-IB
        if ins.mnemonic==Mnemonic.CALL and ins.op0_kind==OpKind.NEAR_BRANCH64:
            out.append((off, ins.near_branch_target-IB,'rel'))
        elif ins.mnemonic==Mnemonic.CALL and ins.op0_kind==OpKind.MEMORY:
            # indirect: try to show mem
            # ES: indirecta: intentar mostrar el operando de memoria
            out.append((off, None, Formatter(FormatterSyntax.INTEL).format(ins)))
    return out
# ES: Desensambla hasta mx instrucciones desde rva mostrando bytes y texto Intel.
# EN: Disassembles up to mx instructions from rva showing bytes and Intel text.
def disasm(rva,length=200,mx=50):
    data=pe.get_data(rva,length); dec=Decoder(64,data,ip=IB+rva); fmt=Formatter(FormatterSyntax.INTEL); o=[];n=0
    for ins in dec:
        if n>=mx:break
        off=ins.ip-IB; b=data[off-rva:off-rva+ins.len].hex()
        o.append(f"0x{off:X}: {b:<22} {fmt.format(ins)}"); n+=1
    return "\n".join(o)
# ES: Busca un patrón AOB (?? o ? = comodín) en .text y devuelve los RVAs que coinciden.
# EN: Searches .text for an AOB pattern (?? or ? = wildcard) and returns the matching RVAs.
def find_aob(pattern):
    toks=pattern.split(); pat=[];mask=[]
    for t in toks:
        if t in('??','?'): pat.append(0);mask.append(False)
        else: pat.append(int(t,16));mask.append(True)
    res=[]
    for name,va,vs in secs:
        if name!='.text':continue
        data=pe.get_data(va,vs);n=len(data);m=len(pat);i=0
        while i<=n-m:
            ok=True
            for j in range(m):
                if mask[j] and data[i+j]!=pat[j]: ok=False;break
            if ok: res.append(va+i)
            i+=1
    return res
# ES: Despacho de los modos calls / d / aob.
# EN: Dispatch of the calls / d / aob modes.
cmd=sys.argv[1]
if cmd=='calls':
    for site,tgt,k in calls(int(sys.argv[2],16), int(sys.argv[3],16) if len(sys.argv)>3 else 0x900):
        if tgt is not None: print(f"  @0x{site:X} -> 0x{tgt:X}")
        else: print(f"  @0x{site:X} -> [indirect] {k}")
elif cmd=='d':
    print(disasm(int(sys.argv[2],16), int(sys.argv[3]) if len(sys.argv)>3 else 200, int(sys.argv[4]) if len(sys.argv)>4 else 50))
elif cmd=='aob':
    r=find_aob(sys.argv[2]); print(f"matches={len(r)}"); [print(f"  0x{x:X}") for x in r[:20]]

# ES: Devuelve los RVAs de los call rel32 (E8) cuyo destino es target_rva.
# EN: Returns the RVAs of rel32 calls (E8) whose target is target_rva.
def callers(target_rva):
    res=[]
    for name,va,vs in secs:
        if name!='.text':continue
        data=pe.get_data(va,vs);n=len(data)
        i=0
        while i<n-5:
            if data[i]==0xE8:
                rel=int.from_bytes(data[i+1:i+5],'little',signed=True)
                tgt=(va+i+5+rel)&0xFFFFFFFF
                if tgt==target_rva:
                    res.append(va+i)
            i+=1
    return res
# ES: Modos callers / refs (se evalúan después de definir callers()).
# EN: callers / refs modes (evaluated after callers() is defined).
if len(sys.argv)>1 and sys.argv[1]=='callers':
    r=callers(int(sys.argv[2],16)); print(f"callers={len(r)}"); [print(f"  0x{x:X}") for x in r[:40]]
elif len(sys.argv)>1 and sys.argv[1]=='refs':
    # ES: busca referencias absolutas lea/mov [imm] a una dirección de .data (p.ej. zona GW+0x750); aproximado buscando el dword
    # find absolute refs lea/mov [imm] to a .data addr (e.g. GW+0x750 region) -- approximate by scanning for the dword
    target=int(sys.argv[2],16)
    cnt=0
    for name,va,vs in secs:
        if name!='.text':continue
        data=pe.get_data(va,vs);n=len(data);i=0
        tb=(target).to_bytes(8,'little')[:4] if target< (1<<32) else None
        # ES: busca los 4 bytes little-endian de (target low32) como desplazamiento; burdo
        # search for 4-byte little-endian of (target low32) as disp; crude
        i=0
        while i<n-4:
            if data[i:i+4]==target.to_bytes(4,'little'):
                print(f"  ref@0x{va+i:X}"); cnt+=1
                if cnt>30:break
            i+=1
        if cnt>30:break

def find_disp(disp_bytes):
    # busca instrucciones que referencian un displacement de 32-bit dado (little endian) en .text
    import struct
    # EN: finds instructions that reference a given 32-bit displacement (little endian) in .text
    res=[]
    target=disp_bytes
    for name,va,vs in secs:
        if name!='.text':continue
        data=pe.get_data(va,vs);n=len(data)
        i=0
        idx=0
        while True:
            j=data.find(target,i)
            if j<0:break
            res.append(va+j)
            i=j+1
            if len(res)>4000:break
    return res
