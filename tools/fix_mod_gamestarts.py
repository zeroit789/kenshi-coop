# ES: Parchea el fichero de datos kenshi-online.mod: en el inicio de partida "Singleplayer" quita la
#     referencia al escuadrón "30-kenshi-online.mod" (el del jugador), que hacía aparecer 20 personajes
#     "Player 1" en toda partida nueva. Baja el contador de escuadrones de 2 a 1, pone a cero la longitud
#     y la cadena de la referencia y guarda una copia .bak. Prueba tres copias del .mod (mods/, data/ y la
#     fuente; rutas relativas a tools/ que pueden no existir). Uso: python fix_mod_gamestarts.py
# EN: Patches the kenshi-online.mod data file: in the "Singleplayer" game start it removes the reference
#     to the "30-kenshi-online.mod" squad (the player's), which spawned 20 "Player 1" characters in every
#     new game. Lowers the squad count from 2 to 1, zeroes the reference length and string and keeps a
#     .bak copy. Tries three copies of the .mod (mods/, data/ and the source; paths relative to tools/
#     that may not exist). Usage: python fix_mod_gamestarts.py

"""
Fix kenshi-online.mod: Remove Player squad references from vanilla game starts.

The mod incorrectly adds "30-kenshi-online.mod" squad to the Singleplayer game start,
causing 20 Player 1 characters to spawn in every new game. This script patches the
squad count from 2 to 1 in the Singleplayer entry, effectively removing the Player squad
while keeping the vanilla squad reference.
"""

import struct
import shutil
import os

# ES: Rutas candidatas del .mod a parchear.
# EN: Candidate .mod paths to patch.
MOD_DIR = os.path.join(os.path.dirname(__file__), '..', 'mods', 'kenshi-online')
MOD_PATH = os.path.join(MOD_DIR, 'kenshi-online.mod')

# ES: Parchear también la copia de data/
# Also patch the copy in data/
DATA_DIR = os.path.join(os.path.dirname(__file__), '..', '..', 'data')
DATA_MOD_PATH = os.path.join(DATA_DIR, 'kenshi-online.mod')

# ES: Parchear también la copia fuente de KenshiMP/ (en realidad ../kenshi-online.mod respecto a tools/)
# Also patch the source copy in KenshiMP/
SRC_MOD_PATH = os.path.join(os.path.dirname(__file__), '..', 'kenshi-online.mod')

# ES: Aplica el parche a un .mod; devuelve True si lo modificó.
# EN: Applies the patch to a .mod; returns True if it modified it.
def patch_mod(path):
    if not os.path.exists(path):
        print(f"  SKIP: {path} not found")
        return False

    with open(path, 'rb') as f:
        data = bytearray(f.read())

    needle = b'30-kenshi-online.mod'

    # ES: Buscar la PRIMERA aparición (lista de escuadrones del inicio Singleplayer)
    # Find the FIRST occurrence (Singleplayer game start's squad list)
    idx = data.find(needle)
    if idx == -1:
        print(f"  SKIP: No '30-kenshi-online.mod' found in {path}")
        return False

    # ES: Estructura antes de la referencia: "squad", contador=2, longitud=20, cadena. Hay que bajar el
    #     contador a 1 y anular la referencia y su prefijo de longitud.
    # The structure before this reference:
    #   05 00 00 00 "squad" 02 00 00 00 14 00 00 00 "30-kenshi-online.mod" ...
    #   "squad" property name, count=2, length=20, reference string
    #
    # We need to find the count byte (02) and change it to 01,
    # AND null out the "30-kenshi-online.mod" reference + its length prefix.

    # ES: Retroceder desde la referencia hasta el contador: contador(4) + longitud(4) + cadena(20), así que
    #     el contador está en idx - 8 y debería valer 02 00 00 00.
    # Walk back from the reference to find the count
    # The structure is: count(4 bytes) + length(4 bytes) + string(20 bytes)
    # So count is at idx - 8, and it should be 02 00 00 00
    count_offset = idx - 8
    count_val = struct.unpack_from('<I', data, count_offset)[0]

    print(f"  Found at offset 0x{idx:04X}, squad count = {count_val}")

    if count_val < 2:
        print(f"  SKIP: Count already {count_val}, nothing to fix")
        return False

    # ES: Cambiar el contador de 2 a 1
    # Change count from 2 to 1
    struct.pack_into('<I', data, count_offset, count_val - 1)

    # ES: Poner a cero el prefijo de longitud (4 bytes antes de la cadena) y la propia cadena
    # Null out the length prefix (4 bytes before the string) and the string itself
    length_offset = idx - 4
    for i in range(length_offset, idx + len(needle)):
        data[i] = 0

    # ES: Relleno tras la cadena: se dejan los ceros (lo más seguro es no desplazar datos).
    # Also fill the gap bytes after the string with zeros (padding that was there)
    # The reference is followed by 12 bytes of zeros then the next reference
    # We need to shift the remaining data or leave the zeros
    # Safest: just zero out the 24 bytes (4 length + 20 string) and leave the zeros

    print(f"  Patched: count {count_val} -> {count_val - 1}, nulled reference at 0x{length_offset:04X}-0x{idx + len(needle):04X}")

    # ES: Copia de seguridad del original (solo la primera vez)
    # Backup original
    backup_path = path + '.bak'
    if not os.path.exists(backup_path):
        shutil.copy2(path, backup_path)
        print(f"  Backup saved to {backup_path}")

    with open(path, 'wb') as f:
        f.write(data)

    print(f"  FIXED: {path}")
    return True

# ES: Punto de entrada: intenta parchear las tres copias e informa.
# EN: Entry point: tries to patch the three copies and reports.
if __name__ == '__main__':
    print("=== Fixing kenshi-online.mod: removing Player squad from vanilla game starts ===\n")

    patched = 0
    for path in [MOD_PATH, DATA_MOD_PATH, SRC_MOD_PATH]:
        print(f"Checking: {path}")
        if patch_mod(path):
            patched += 1
        print()

    if patched > 0:
        print(f"Done! Patched {patched} file(s). Player characters will no longer spawn in vanilla game starts.")
        print("The 'Multiplayer' game start still works correctly.")
    else:
        print("No files needed patching.")
