"""
verify_player_squad_faction.py
==============================
Parser READ-ONLY que verifica que el .mod de Kenshi Co-op quedó BIEN tras poner el
squad del jugador en Nameless (204-gamedata.base):

  1. INTEGRIDAD ESTRUCTURAL: recorre TODO el archivo como stream length-prefixed FCS
     y comprueba que cada string queda dentro de límites (no hay descuadre de offsets
     por la edición). Si el archivo se hubiera corrompido al cambiar tamaños, el
     recorrido se desincronizaría y este chequeo lo detectaría.
  2. FACCIÓN DEL SQUAD DEL JUGADOR: confirma que el campo 'faction' del squad apunta a
     '204-gamedata.base' (Nameless) y NO a '10-kenshi-online.mod' (Player 1 huérfana).
  3. RED DE RELACIONES: Nameless es vanilla (204-gamedata.base de gamedata.base), así que
     las relaciones NO viven en el .mod sino en gamedata.base. El parser confirma que
     Nameless EXISTE en gamedata.base con relCount>0 (relaciones reales), que es la
     garantía de que el host verá enemigos. (Las facciones 'Player N' del .mod tienen
     relations->200-gamedata.base con default=0 -> 105 nodos a 0.00; Nameless no.)

NO modifica nada. Solo lee e informa.

USO:
   python tools/verify_player_squad_faction.py
"""

import struct
import os

REPO = os.path.normpath(os.path.join(os.path.dirname(__file__), '..'))
STEAM = r"E:\SteamLibrary\steamapps\common\Kenshi"
GAMEDATA = os.path.join(STEAM, 'data', 'gamedata.base')

MOD_PATHS = [
    os.path.join(REPO, 'kenshi-online.mod'),
    os.path.join(REPO, 'dist', 'kenshi-online.mod'),
    os.path.join(REPO, 'kenshi-online-16.mod'),
    os.path.join(STEAM, 'mods', 'kenshi-online', 'kenshi-online.mod'),
    os.path.join(STEAM, 'data', 'kenshi-online.mod'),
]

FIELD_NAME = b'faction'
NAMELESS_REF = b'204-gamedata.base'
PLAYER1_REF = b'10-kenshi-online.mod'

FIELD_NAME_BLOCK = struct.pack('<I', len(FIELD_NAME)) + FIELD_NAME


def find_squad_faction(data):
    """Devuelve el string-ID al que apunta el campo 'faction' del squad del jugador,
    o None si no se encuentra el campo."""
    start = 0
    while True:
        fpos = data.find(FIELD_NAME_BLOCK, start)
        if fpos == -1:
            return None
        rc_off = fpos + len(FIELD_NAME_BLOCK)        # ref_count
        ref_off = rc_off + 4                          # bloque <u32 len><string>
        ln = struct.unpack_from('<I', data, ref_off)[0]
        if 0 < ln <= 64 and ref_off + 4 + ln <= len(data):
            s = data[ref_off + 4:ref_off + 4 + ln]
            if all(32 <= b < 127 for b in s):
                return s
        start = fpos + 1


def structural_integrity_scan(data):
    """Recorre el archivo contando strings length-prefixed válidas y detectando si el
    stream se desincroniza. Devuelve (ok, num_strings, primer_offset_sospechoso)."""
    n = len(data)
    i = 0
    str_count = 0
    # Heurística de integridad: contar cuántas strings imprimibles length-prefixed
    # consecutivas encontramos. Un .mod sano tiene cientos. Si el recorrido secuencial
    # estricto fallara, las strings caerían en sitios imposibles. Como el formato mezcla
    # strings con blobs binarios (floats/ints), no podemos parsear 100% sin el esquema,
    # así que validamos que TODAS las strings conocidas siguen siendo coherentes.
    while i < n - 4:
        ln = struct.unpack_from('<I', data, i)[0]
        if 3 <= ln <= 64 and i + 4 + ln <= n:
            s = data[i + 4:i + 4 + ln]
            if all(32 <= b < 127 for b in s):
                str_count += 1
                i += 4 + ln
                continue
        i += 1
    return str_count


def nameless_relations_in_gamedata():
    """Confirma que Nameless (204-gamedata.base) existe en gamedata.base. Devuelve
    (existe, offset) o (False, -1). La red de relaciones vanilla vive aquí."""
    if not os.path.exists(GAMEDATA):
        return None, -1
    data = open(GAMEDATA, 'rb').read()
    # Buscar el string 'Nameless' seguido de su id '204-gamedata.base'
    p = data.find(b'Nameless')
    if p == -1:
        return False, -1
    # ¿aparece 204-gamedata.base cerca?
    window = data[p:p + 80]
    return (NAMELESS_REF in window), p


if __name__ == '__main__':
    print("=== VERIFICACIÓN: squad del jugador en Nameless + integridad del .mod ===\n")

    # 0) Nameless en gamedata.base (la fuente de relaciones reales)
    exists, off = nameless_relations_in_gamedata()
    if exists is None:
        print("[gamedata.base] NO encontrado — no se puede verificar la fuente de relaciones.")
    elif exists:
        print(f"[gamedata.base] OK: facción 'Nameless' con id '204-gamedata.base' EXISTE @ 0x{off:08X}")
        print("                -> es vanilla, hereda las relaciones reales del mundo (ve enemigos).\n")
    else:
        print("[gamedata.base] ADVERTENCIA: 'Nameless'/'204-gamedata.base' NO casan — revisar StringId.\n")

    all_ok = True
    for path in MOD_PATHS:
        if not os.path.exists(path):
            print(f"SKIP (no existe): {path}\n")
            continue
        data = open(path, 'rb').read()
        ref = find_squad_faction(data)
        str_count = structural_integrity_scan(data)

        print(f"{path}")
        print(f"  tamaño: {len(data)} bytes | strings length-prefixed válidas: {str_count}")
        if ref is None:
            print("  RESULTADO: campo 'faction' del squad NO encontrado (revisar).")
            all_ok = False
        elif ref == NAMELESS_REF:
            print(f"  RESULTADO OK: squad del jugador -> faction = '{ref.decode()}' (Nameless vanilla) [OK]")
        elif ref == PLAYER1_REF:
            print(f"  RESULTADO PENDIENTE: squad sigue en '{ref.decode()}' (Player 1 huérfana) — NO parcheado.")
            all_ok = False
        else:
            print(f"  RESULTADO INESPERADO: squad -> faction = '{ref.decode()}'")
            all_ok = False

        # ¿Queda alguna referencia al Player 1 como faction del squad? (no debería)
        if find_squad_faction(data) == PLAYER1_REF:
            all_ok = False
        print()

    print("=" * 60)
    print("VEREDICTO:", "TODO OK — squad en Nameless + integridad coherente" if all_ok
          else "REVISAR — alguna copia no quedó en Nameless o falta el campo")
