# -*- coding: utf-8 -*-
"""
RE del SISTEMA DE TAREAS/ORDENES del Character de Kenshi (Steam 1.0.68).
Desensambla AI::create (0x622110), localiza vtable de Tasker (RTTI 0x1CF6F50)
y GOAPTaskMgr (RTTI 0x1CDE7C0), y sigue offsets char+0xNNN -> AICore -> cola.

Usa iced-x86 (capstone bloqueado por WDAC). Parser PE propio.
"""
import struct, sys, os
from iced_x86 import Decoder, Formatter, FormatterSyntax, Mnemonic, OpKind, Code

EXE = r"E:\SteamLibrary\steamapps\common\Kenshi\kenshi_x64.exe"
BASE = 0x140000000

class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        self.base = BASE
        self.sections = []
        self._parse()
    def _parse(self):
        e = struct.unpack_from("<I", self.data, 0x3C)[0]
        coff = e + 4
        nsec = struct.unpack_from("<H", self.data, coff + 2)[0]
        ohs = struct.unpack_from("<H", self.data, coff + 16)[0]
        opt = coff + 20
        if struct.unpack_from("<H", self.data, opt)[0] == 0x20b:
            self.base = struct.unpack_from("<Q", self.data, opt + 24)[0]
        soff = opt + ohs
        for i in range(nsec):
            o = soff + i*40
            name = self.data[o:o+8].rstrip(b"\x00").decode("ascii","ignore")
            vsize = struct.unpack_from("<I", self.data, o+8)[0]
            rva = struct.unpack_from("<I", self.data, o+12)[0]
            rs = struct.unpack_from("<I", self.data, o+16)[0]
            ro = struct.unpack_from("<I", self.data, o+20)[0]
            self.sections.append(dict(name=name,vsize=vsize,rva=rva,rs=rs,ro=ro))
    def rva2off(self, rva):
        for s in self.sections:
            if s["rva"] <= rva < s["rva"] + max(s["rs"], s["vsize"]):
                if rva - s["rva"] < s["rs"]:
                    return rva - s["rva"] + s["ro"]
        return None
    def off2rva(self, off):
        for s in self.sections:
            if s["ro"] <= off < s["ro"] + s["rs"]:
                return off - s["ro"] + s["rva"]
        return None
    def read_q(self, rva):
        o = self.rva2off(rva)
        if o is None or o+8 > len(self.data): return None
        return struct.unpack_from("<Q", self.data, o)[0]
    def sec(self, name):
        for s in self.sections:
            if s["name"] == name: return s
        return None

pe = PE(EXE)
FMT = Formatter(FormatterSyntax.INTEL)
FMT.hex_prefix = "0x"; FMT.hex_suffix = ""

def disasm(rva, length=400):
    off = pe.rva2off(rva)
    if off is None:
        return []
    code = pe.data[off:off+length]
    dec = Decoder(64, code, ip=BASE+rva)
    out = []
    for ins in dec:
        s = FMT.format(ins)
        start = pe.rva2off(ins.ip - BASE)
        raw = pe.data[start:start+ins.len]
        out.append((ins.ip - BASE, raw, s, ins))
        if ins.code == Code.INVALID: break
    return out

def print_disasm(rva, length=400, label="", stop_on_ret=False):
    print(f"\n===== {label}  RVA 0x{rva:X} =====")
    for r, raw, s, ins in disasm(rva, length):
        hexb = " ".join(f"{b:02X}" for b in raw)
        print(f"  0x{r:08X}: {hexb:<26} {s}")
        if stop_on_ret and ins.mnemonic == Mnemonic.RET:
            break

def call_target(ins):
    if ins.mnemonic == Mnemonic.CALL and ins.op0_kind == OpKind.NEAR_BRANCH64:
        return ins.near_branch_target - BASE
    return None

print("="*72)
print("Secciones:")
for s in pe.sections:
    print(f"  {s['name']:8s} rva=0x{s['rva']:08X} vsize=0x{s['vsize']:X} raw=0x{s['rs']:X}")

# ════════════════════════════════════════════════════════════════════
# PASO 0: RTTI -> vtable (Tasker, GOAPTaskMgr, Task_*)
# ════════════════════════════════════════════════════════════════════
print("\n" + "="*72)
print("PASO 0: RTTI -> vtable (CompleteObjectLocator -> TypeDescriptor)")
print("="*72)

def find_vtables_for_typedescriptor(td_rva):
    rdata = pe.sec(".rdata")
    if not rdata: return []
    rstart = rdata["ro"]; rsize = rdata["rs"]; rrva = rdata["rva"]
    blob = pe.data[rstart:rstart+rsize]
    cols = []
    for i in range(0, len(blob)-24, 4):
        sig = struct.unpack_from("<I", blob, i)[0]
        if sig not in (0,1): continue
        ptd = struct.unpack_from("<I", blob, i+12)[0]
        if ptd == td_rva:
            cols.append(rrva + i)
    vtables = []
    for col_rva in cols:
        target = struct.pack("<Q", BASE + col_rva)
        idx = 0
        while True:
            p = blob.find(target, idx)
            if p == -1: break
            vtables.append((rrva + p + 8, col_rva))
            idx = p + 1
    return vtables

# Las RVAs dadas apuntan al campo `name` (mangled string) DENTRO del TypeDescriptor.
# El TypeDescriptor MSVC x64 = { void* pVFTable; void* spare; char name[] }
# => TD_real = name_rva - 0x10
RTTI = [("Tasker",0x1CF6F50),("GOAPTaskMgr",0x1CDE7C0),
        ("Task_MeleeAttack",0x1CF7350),("Task_GetUp",0x1CF7538),
        ("Task_GetOutOfBed",0x1CF76D8),("Task_Runaway",0x1CF79E0),
        ("CombatState",0x1C93880),("AttackState",0x1C93908)]
VT = {}
for name, name_rva in RTTI:
    td = name_rva - 0x10  # TypeDescriptor real
    nm_off = pe.rva2off(name_rva)
    nm = ""
    if nm_off:
        end = pe.data.find(b"\x00", nm_off)
        nm = pe.data[nm_off:end].decode("ascii","replace")
    vts = find_vtables_for_typedescriptor(td)
    print(f"\n[{name}] TD=0x{td:X} name='{nm}'  vtables={len(vts)}")
    for vt_rva, col_rva in vts:
        first = pe.read_q(vt_rva); fr=(first-BASE) if first else 0
        print(f"   vtable RVA 0x{vt_rva:X} (COL 0x{col_rva:X}) slot0->0x{fr:X}")
        VT.setdefault(name, []).append(vt_rva)

print("\n[OK] PASO 0 hecho")

VT_TASKER = 0x16BDC68
VT_GOAP   = 0x16BC1D8

# ════════════════════════════════════════════════════════════════════
# PASO 1: AI::create (0x622110) -> offset char+0xNNN del AICore/Tasker
# ════════════════════════════════════════════════════════════════════
print("\n" + "="*72)
print("PASO 1: AI::create 0x622110 - desensamblado completo")
print("="*72)
print_disasm(0x622110, length=900, label="AI::create")

# Localizar calls (constructores/allocadores) y los `mov [reg+disp], rax` que les siguen
print("\n--- Analisis de stores tras CALL (candidatos a 'char+0xNNN = AICore') ---")
ins_list = disasm(0x622110, 900)
for i,(r,raw,s,ins) in enumerate(ins_list):
    ct = call_target(ins)
    if ct is not None:
        # mirar las ~6 instrucciones siguientes por un store de rax a [reg+disp]
        for j in range(i+1, min(i+8, len(ins_list))):
            r2,raw2,s2,ins2 = ins_list[j]
            if ins2.mnemonic == Mnemonic.MOV and ins2.op0_kind == OpKind.MEMORY \
               and ins2.op1_kind == OpKind.REGISTER:
                print(f"  call 0x{ct:X} @0x{r:X}  =>  store @0x{r2:X}: {s2}")
                break

print("\n[OK] PASO 1 hecho")

# ════════════════════════════════════════════════════════════════════
# PASO 2: constructor del objeto char+0x20 (call 0x184F3 -> destino real)
#   nota: 0x184F3 es un thunk; resolver el jmp/llamada interna.
# ════════════════════════════════════════════════════════════════════
print("\n" + "="*72)
print("PASO 2: constructor del AICore (char+0x20) y que vtable instala")
print("="*72)
print_disasm(0x184F3, length=64, label="thunk 0x184F3", stop_on_ret=True)
# resolver el jmp del thunk
for r,raw,s,ins in disasm(0x184F3, 32):
    if ins.mnemonic == Mnemonic.JMP and ins.op0_kind == OpKind.NEAR_BRANCH64:
        real = ins.near_branch_target - BASE
        print(f"  thunk -> 0x{real:X}")
        print_disasm(real, length=700, label="AICore::ctor (real)")
        # buscar instalacion de vtable (lea rax,[vt]; mov [reg],rax) y stores de subobjetos
        print("\n  --- vtables instaladas / stores de punteros en el ctor ---")
        cl = disasm(real, 700)
        for i,(rr,rb,ss,ii) in enumerate(cl):
            if ii.mnemonic == Mnemonic.LEA and ii.op1_kind == OpKind.MEMORY and ii.memory_base == 0:
                tgt = ii.memory_displacement - BASE if ii.memory_displacement>=BASE else ii.memory_displacement
                # ¿es una vtable conocida?
                tag = ""
                if tgt == VT_TASKER: tag = "  <<< vtable TASKER"
                elif tgt == VT_GOAP: tag = "  <<< vtable GOAPTaskMgr"
                if 0x1673000 <= tgt < 0x1BBE000:  # .rdata => posible vtable
                    print(f"   @0x{rr:X}: {ss}  (.rdata 0x{tgt:X}){tag}")
            ct = call_target(ii)
            if ct is not None:
                print(f"   @0x{rr:X}: call 0x{ct:X}")
        break

print("\n[OK] PASO 2 hecho")

# ════════════════════════════════════════════════════════════════════
# PASO 3: identificar la clase del objeto char+0x20 via RTTI de su vtable
#         y de las vtables de sus sub-objetos.
# ════════════════════════════════════════════════════════════════════
print("\n" + "="*72)
print("PASO 3: RTTI de las vtables instaladas (vtable -> COL -> TypeDescriptor -> nombre)")
print("="*72)

def vtable_to_classname(vt_rva):
    """vtable[-1] -> COL (RVA en imagen) -> +0xC (TypeDescriptor RVA) -> +0x10 name."""
    col_va = pe.read_q(vt_rva - 8)
    if not col_va or not (BASE <= col_va < BASE+0x3000000):
        return None, None
    col_rva = col_va - BASE
    o = pe.rva2off(col_rva)
    if o is None: return None, col_rva
    ptd = struct.unpack_from("<I", pe.data, o+12)[0]  # TypeDescriptor RVA (campo +0xC del COL)
    nm_off = pe.rva2off(ptd + 0x10)
    if nm_off is None: return None, col_rva
    end = pe.data.find(b"\x00", nm_off)
    return pe.data[nm_off:end].decode("ascii","replace"), col_rva

VTS_TO_NAME = {
    "char+0x20 (AICore obj)": 0x16E3F30,
    "subobj @+0x2E8":          0x16E34D0,
    "subobj @+0x308/+0x340":   0x16E3EB8,
    "ctor2 vtable (0x50DD80)": 0x16E3F30,
}
for tag, vt in VTS_TO_NAME.items():
    nm, col = vtable_to_classname(vt)
    print(f"  {tag:28s} vtable 0x{vt:X} -> {nm}")

# Tambien: ¿la vtable Tasker/GOAP aparece como puntero dentro del rango del objeto char+0x20?
# Escaneamos el ctor por lea de cualquier vtable .rdata y la nombramos.
print("\n--- TODAS las vtables .rdata cargadas por lea en el ctor 0x50DB70 ---")
seen=set()
for r,raw,s,ins in disasm(0x50DB70, 700):
    if ins.mnemonic == Mnemonic.LEA and ins.is_ip_rel_memory_operand:
        tgt = ins.ip_rel_memory_address - BASE
        if 0x1673000 <= tgt < 0x1BBE000 and tgt not in seen:
            seen.add(tgt)
            nm,_ = vtable_to_classname(tgt)
            print(f"   @0x{r:X} lea ->0x{tgt:X}  class={nm}")

print("\n[OK] PASO 3 hecho")

# ════════════════════════════════════════════════════════════════════
# PASO 4: la cola de tareas. Buscar donde se usa la vtable Tasker/GOAP
#         y rastrear xrefs (lea/mov) a esas vtables en .text para hallar
#         constructores que las instalan, y el offset dentro del char/AICore.
# ════════════════════════════════════════════════════════════════════
print("\n" + "="*72)
print("PASO 4: xrefs a vtable Tasker(0x16BDC68) y GOAP(0x16BC1D8) en .text")
print("="*72)

def find_lea_xrefs_to_vtable(vt_rva, limit=40):
    """Busca instrucciones lea reg,[vt_rva] en .text (instalacion de vtable)."""
    text = pe.sec(".text")
    tstart = text["ro"]; tsize=text["rs"]; trva=text["rva"]
    vt_va = BASE + vt_rva
    hits=[]
    blob = pe.data[tstart:tstart+tsize]
    # patron lea: 48 8D xx [RIP+disp32] (7 bytes). resolvemos disp.
    for i in range(len(blob)-7):
        if blob[i] in (0x48,0x4C) and blob[i+1]==0x8D:
            modrm=blob[i+2];
            if (modrm>>6)==0 and (modrm&7)==5:
                disp=struct.unpack_from("<i",blob,i+3)[0]
                instr_rva = trva + i
                tgt = instr_rva + 7 + disp
                if tgt == vt_rva:
                    hits.append(instr_rva)
                    if len(hits)>=limit: break
    return hits

for nm, vt in [("Tasker", VT_TASKER), ("GOAPTaskMgr", VT_GOAP)]:
    hits = find_lea_xrefs_to_vtable(vt)
    print(f"\n[{nm}] vtable 0x{vt:X}: {len(hits)} sitios 'lea reg,[vtable]' (ctors)")
    for h in hits[:8]:
        print(f"   ctor instala vtable @ RVA 0x{h:X}")
        # mostrar la instruccion siguiente (mov [reg+0],rax => offset 0 normalmente)
        nxt = disasm(h, 24)
        for r,raw,s,ins in nxt[:3]:
            print(f"       0x{r:X}: {s}")

print("\n[OK] PASO 4 hecho")

# ════════════════════════════════════════════════════════════════════
# PASO 5: layout del lektor<Tasker*> (la cola) y AITaskSytem vtable slots.
#   - vtable lektor = 0x16E34D0. Desensamblar sus primeros metodos.
#   - vtable AITaskSytem = 0x16E3F30. Listar slots (para addTask).
# ════════════════════════════════════════════════════════════════════
print("\n" + "="*72)
print("PASO 5: vtable AITaskSytem (0x16E3F30) - slots y metodos")
print("="*72)
def dump_vtable(vt_rva, n=40, name=""):
    print(f"\n  vtable {name} 0x{vt_rva:X}:")
    for i in range(n):
        fp = pe.read_q(vt_rva + i*8)
        if fp is None: break
        frva = fp - BASE if fp and BASE<=fp<BASE+0x3000000 else None
        if frva is None or not (0x1000 <= frva < 0x1673000):
            # fin de vtable (no apunta a .text)
            if i>0: break
        # primer mnemonic del metodo
        first=""
        d=disasm(frva, 8)
        if d: first=d[0][2]
        print(f"   [{i:2d}] slot+0x{i*8:X}  -> 0x{frva:X}   {first}")

dump_vtable(0x16E3F30, 48, "AITaskSytem")

print("\n" + "="*72)
print("PASO 5b: vtable lektor<Tasker*> (0x16E34D0) - layout del contenedor")
print("="*72)
dump_vtable(0x16E34D0, 16, "lektor<Tasker*>")

# Desensamblar un metodo del lektor que devuelva tamano/iteradores.
# El lektor de Kenshi suele tener layout: [vtable][?][begin ptr][end ptr][capacity ptr]...
# Mostramos varios metodos cortos para inferir offsets.
print("\n  --- Metodos del lektor (slots 1..6) ---")
for i in range(1,7):
    fp = pe.read_q(0x16E34D0 + i*8)
    if not fp: continue
    frva=fp-BASE
    if not (0x1000<=frva<0x1673000): continue
    print(f"\n  [lektor slot {i}] 0x{frva:X}:")
    for r,raw,s,ins in disasm(frva, 80):
        print(f"     0x{r:X}: {s}")
        if ins.mnemonic==Mnemonic.RET: break

print("\n[OK] PASO 5 hecho")

# ════════════════════════════════════════════════════════════════════
# PASO 6: layout de datos del lektor (destructor revela begin/end/cap),
#         y resolver thunks de la vtable AITaskSytem.
# ════════════════════════════════════════════════════════════════════
print("\n" + "="*72)
print("PASO 6: destructor lektor (0x50ADC0) -> layout begin/end/capacity")
print("="*72)
def follow_thunk(rva):
    d = disasm(rva, 16)
    if d and d[0][3].mnemonic==Mnemonic.JMP and d[0][3].op0_kind==OpKind.NEAR_BRANCH64:
        return d[0][3].near_branch_target - BASE
    return rva
real = follow_thunk(0x50ADC0)
print_disasm(real, 220, label="lektor::~dtor (free de la cola)", stop_on_ret=True)

print("\n--- vtable AITaskSytem: thunks resueltos + primeras instrucciones ---")
for i in range(6):
    fp=pe.read_q(0x16E3F30+i*8)
    if not fp: continue
    frva=follow_thunk(fp-BASE)
    print(f"\n  [slot {i}] real 0x{frva:X}:")
    for r,raw,s,ins in disasm(frva, 70):
        print(f"     0x{r:X}: {s}")
        if ins.mnemonic==Mnemonic.RET: break

print("\n[OK] PASO 6 hecho")

# ════════════════════════════════════════════════════════════════════
# PASO 7: AItaskSytem::ctor por AI::create -> confirmar char+0x20 y
#         buscar el metodo addTask (push al lektor<Tasker*> +0x2E8).
#   Heuristica: funciones que hacen mov rcx,[obj+0x2E8] o lea rcx,[obj+0x2E8]
#   y luego un push_back (call con store de Tasker*).
# ════════════════════════════════════════════════════════════════════
print("\n" + "="*72)
print("PASO 7: accesos a AITaskSytem+0x2E8 (la cola lektor<Tasker*>)")
print("="*72)
text=pe.sec(".text"); tstart=text["ro"]; tsize=text["rs"]; trva=text["rva"]
blob=pe.data[tstart:tstart+tsize]
# buscar disp32 == 0x2E8 en lea/mov [reg+0x2E8] (modrm mod=10)
import re
hits=[]
needle=struct.pack("<i",0x2E8)
idx=0
while True:
    p=blob.find(needle, idx)
    if p==-1: break
    # comprobar que el byte previo forma un modrm mod=10 (disp32) tras 48 8B/8D/89
    if p>=3:
        op=blob[p-3]; opc=blob[p-2]; modrm=blob[p-1]
        if op in (0x48,0x4C) and opc in (0x8B,0x8D,0x89) and (modrm>>6)==2:
            rva=trva+(p-3)
            hits.append((rva, opc))
    idx=p+1
print(f"  {len(hits)} accesos [reg+0x2E8] en .text (incluye otros structs con ese offset)")
for rva,opc in hits[:30]:
    d=disasm(rva,16)
    if d:
        print(f"   0x{rva:X}: {d[0][2]}")

print("\n[OK] PASO 7 hecho")





