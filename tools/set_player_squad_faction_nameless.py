"""
set_player_squad_faction_nameless.py
=====================================
Cambia la FACCIÓN del squad del jugador del gamestart 'Multiplayer' del .mod
de Kenshi Co-op de la facción huérfana "Player 1" (10-kenshi-online.mod) a la
facción VANILLA "Nameless" (204-gamedata.base), que SÍ tiene la red de relaciones
real heredada (ve enemigos -> combate nativo; co-op aliados entre sí).

POR QUÉ
-------
El squad "Player 1 squad" (id 11-kenshi-online.mod), que el gamestart 'Multiplayer'
instancia para el host, tiene su campo 'faction' apuntando a "Player 1"
(10-kenshi-online.mod). Esa facción nace con 105 relaciones a 0.00 (clon huérfano de
ModGen) -> el host no ve a nadie como enemigo -> el gate isAlly del encolador de ataque
(RVA 0x6744A0) no encola el ataque -> amIdle=1 -> no combate. Poniendo el squad en
Nameless (204-gamedata.base) el host hereda las relaciones vanilla reales.

SEGURIDAD DEL FORMATO FCS
-------------------------
El .mod es un stream serializado lineal length-prefixed SIN tabla de offsets absolutos
(verificado por RE: la cabecera son flags + dependencias, no hay índice de offsets; y
rebase_mod.py ya cambió longitudes de strings en TODO el archivo y produjo un .mod que el
juego carga y juega). Por tanto reconstruir el buffer al cambiar el tamaño de UN string
es seguro: nada referencia por posición de byte, solo por nombre de string.

QUÉ CAMBIA EXACTAMENTE (en el .mod de 2 jugadores)
--------------------------------------------------
  - El string 'faction' del objeto "Player 1 squad" (id 11-kenshi-online.mod).
  - Referencia ANTES: prefijo LE=20 + "10-kenshi-online.mod"  (24 bytes)
  - Referencia DESPUÉS: prefijo LE=17 + "204-gamedata.base"     (21 bytes)  -> -3 bytes
  - ref_count (=1) del campo 'faction' NO se toca (no añadimos/quitamos referencias).
  - El objeto Faction "Player 1" (id 10-kenshi-online.mod) se deja DEFINIDO pero
    ya no lo usa el squad del jugador (queda huérfano benigno; no estorba).

ESTRATEGIA DE LOCALIZACIÓN (robusta, no por offset fijo)
--------------------------------------------------------
Busca el patrón de bytes del campo 'faction' del squad del jugador:
   "faction"(len-prefixed) + ref_count(uint32=1) + len-prefixed("10-kenshi-online.mod")
y reemplaza SOLO la referencia (prefijo + string) por Nameless. Así funciona aunque los
offsets cambien entre la versión de 2 y de 16 jugadores.

USO
---
   python tools/set_player_squad_faction_nameless.py            # aplica a todas las copias
   python tools/set_player_squad_faction_nameless.py --dry-run  # solo informa, no escribe

NO despliega a Steam. Zero despliega manualmente tras probar.
"""

import struct
import os
import sys
import shutil

# ── Constantes del formato ────────────────────────────────────────────────────
FIELD_NAME      = b'faction'                 # nombre del campo de referencia a tocar
OLD_FACTION_REF = b'10-kenshi-online.mod'    # facción Player 1 (huérfana), 20 chars
NEW_FACTION_REF = b'204-gamedata.base'       # facción Nameless (vanilla), 17 chars

# Patrón a localizar: <u32 len=7>"faction"<u32 ref_count=1><u32 len=20>"10-kenshi-online.mod"
# Lo construimos por piezas para tolerar variaciones de ref_count.
FIELD_NAME_BLOCK = struct.pack('<I', len(FIELD_NAME)) + FIELD_NAME            # 'faction' length-prefixed
OLD_REF_BLOCK    = struct.pack('<I', len(OLD_FACTION_REF)) + OLD_FACTION_REF  # ref vieja length-prefixed
NEW_REF_BLOCK    = struct.pack('<I', len(NEW_FACTION_REF)) + NEW_FACTION_REF  # ref nueva length-prefixed

# ── Lista de copias del .mod a parchear ───────────────────────────────────────
REPO = os.path.normpath(os.path.join(os.path.dirname(__file__), '..'))
STEAM = r"E:\SteamLibrary\steamapps\common\Kenshi"

MOD_PATHS = [
    os.path.join(REPO, 'kenshi-online.mod'),                       # raíz repo (2 jug)
    os.path.join(REPO, 'dist', 'kenshi-online.mod'),               # dist (2 jug)
    os.path.join(REPO, 'kenshi-online-16.mod'),                    # 16 jugadores
    os.path.join(STEAM, 'mods', 'kenshi-online', 'kenshi-online.mod'),  # desplegado mods/
    os.path.join(STEAM, 'data', 'kenshi-online.mod'),              # desplegado data/
]


def patch_mod(path, dry_run=False):
    """Cambia la facción del squad del jugador a Nameless en UNA copia del .mod.
    Devuelve True si parcheó, False si no aplicaba o ya estaba parcheado."""
    if not os.path.exists(path):
        print(f"  SKIP (no existe): {path}")
        return False

    data = open(path, 'rb').read()
    size_before = len(data)

    # 1) ¿Ya está parcheado? (el squad del jugador ya apunta a Nameless)
    #    Buscamos el bloque 'faction' + ref_count + NEW_REF.
    already = _find_field_ref(data, NEW_REF_BLOCK)
    if already is not None:
        print(f"  YA PARCHEADO (squad del jugador ya en Nameless): {path}")
        return False

    # 2) Localizar el campo 'faction' del squad del jugador apuntando a Player 1.
    hit = _find_field_ref(data, OLD_REF_BLOCK)
    if hit is None:
        print(f"  SKIP (no se halló el campo 'faction'->'10-kenshi-online.mod'): {path}")
        return False

    ref_block_off = hit  # offset del bloque <u32 len><string> de la referencia vieja
    # Reconstruir el buffer: parte previa + nueva referencia + parte posterior.
    new_data = data[:ref_block_off] + NEW_REF_BLOCK + data[ref_block_off + len(OLD_REF_BLOCK):]
    size_after = len(new_data)
    delta = size_after - size_before

    print(f"  Campo 'faction' del squad del jugador hallado @ 0x{ref_block_off:05X}")
    print(f"  Cambio: '10-kenshi-online.mod'(20) -> '204-gamedata.base'(17)  | tamaño {size_before} -> {size_after} ({delta:+d})")

    # 3) Verificación de cordura: el archivo debe encoger exactamente 3 bytes.
    expected_delta = len(NEW_REF_BLOCK) - len(OLD_REF_BLOCK)  # = -3
    if delta != expected_delta:
        print(f"  ABORTADO: delta inesperado ({delta}, esperado {expected_delta}). NO se escribe.")
        return False

    # 4) Verificación: la referencia vieja del squad ya no debe quedar como faction del squad.
    if _find_field_ref(new_data, OLD_REF_BLOCK) is not None:
        # Puede quedar OTRA referencia 'faction'->Player1 (p.ej. en otro objeto), pero
        # para el squad del jugador concreto ya la cambiamos. Avisamos si hay más.
        print(f"  NOTA: queda al menos otro campo 'faction'->'10-kenshi-online.mod' en el archivo "
              f"(otro objeto). Revisar si era esperado.")

    if dry_run:
        print(f"  [DRY-RUN] no se escribe.")
        return True

    # 5) Backup .bak-pre-nameless si no existe, y escribir.
    backup = path + '.bak-pre-nameless'
    if not os.path.exists(backup):
        shutil.copy2(path, backup)
        print(f"  Backup -> {backup}")

    open(path, 'wb').write(new_data)
    print(f"  ESCRITO: {path}")
    return True


def _find_field_ref(data, ref_block):
    """Localiza el bloque de referencia (ref_block = <u32 len><string>) que sigue al
    campo 'faction' + ref_count(uint32). Devuelve el offset del ref_block, o None.
    Esto evita confundir con otras apariciones del string en el archivo: solo casa
    cuando la referencia está inmediatamente precedida por 'faction' + un ref_count."""
    start = 0
    while True:
        fpos = data.find(FIELD_NAME_BLOCK, start)
        if fpos == -1:
            return None
        # Tras 'faction'(len-prefixed) viene ref_count(uint32) y luego el ref_block.
        rc_off = fpos + len(FIELD_NAME_BLOCK)
        ref_off = rc_off + 4
        if data[ref_off:ref_off + len(ref_block)] == ref_block:
            return ref_off
        start = fpos + 1


if __name__ == '__main__':
    dry = '--dry-run' in sys.argv
    print("=== Poner el squad del jugador (gamestart Multiplayer) en Nameless (204-gamedata.base) ===")
    print(f"    Modo: {'DRY-RUN (no escribe)' if dry else 'APLICAR'}\n")
    patched = 0
    for p in MOD_PATHS:
        print(f"Checking: {p}")
        if patch_mod(p, dry_run=dry):
            patched += 1
        print()
    print(f"Hecho. {patched} archivo(s) {'parcheables' if dry else 'parcheados'}.")
    if not dry and patched:
        print("Recuerda: NO se ha desplegado nada nuevo. Verifica en el juego con partida NUEVA.")
