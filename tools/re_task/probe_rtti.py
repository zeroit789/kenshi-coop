# -*- coding: utf-8 -*-
"""Sondea qué hay EXACTAMENTE en las RVAs RTTI dadas: ¿TypeDescriptor, vtable o COL?
Lee bytes crudos y busca strings .?AV cercanos."""
import struct
from re_task_system import pe, BASE

CANDS = [("Tasker",0x1CF6F50),("GOAPTaskMgr",0x1CDE7C0),
         ("Task_MeleeAttack",0x1CF7350),("Task_GetUp",0x1CF7538),
         ("Task_GetOutOfBed",0x1CF76D8),("Task_Runaway",0x1CF79E0)]

def hexat(rva, n=64):
    o = pe.rva2off(rva)
    if o is None: return "(no map)"
    return " ".join(f"{b:02X}" for b in pe.data[o:o+n])

def ascii_at(rva, n=64):
    o = pe.rva2off(rva)
    if o is None: return "(no map)"
    b = pe.data[o:o+n]
    return "".join(chr(c) if 32<=c<127 else "." for c in b)

for name, rva in CANDS:
    print(f"\n##### {name}  RVA 0x{rva:X}")
    print("  bytes :", hexat(rva, 48))
    print("  ascii :", ascii_at(rva, 48))
    # qué sección
    for s in pe.sections:
        if s["rva"] <= rva < s["rva"]+max(s["rs"],s["vsize"]):
            print(f"  seccion: {s['name']}")
    # ¿es un puntero (vtable entry)? leer qword
    q = pe.read_q(rva)
    if q and BASE <= q < BASE+0x3000000:
        print(f"  qword[0] = 0x{q-BASE:X} (posible RVA en imagen)")
    # buscar string .?AV en una ventana alrededor
    o = pe.rva2off(rva)
    if o:
        win = pe.data[o-0x20:o+0x80]
        idx = win.find(b".?A")
        if idx >= 0:
            end = win.find(b"\x00", idx)
            print(f"  string .?A cerca (off {idx-0x20:+d}): {win[idx:end].decode('ascii','replace')}")
