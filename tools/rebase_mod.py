# ES: Renombra el mod base "multiplayr.mod" a "kenshi-online.mod" dentro del fichero de datos FCS: cada
#     cadena "XX-multiplayr.mod" (con prefijo de longitud u32 LE) pasa a "XX-kenshi-online.mod"
#     actualizando el prefijo (+3). Uso: python rebase_mod.py (rutas fijas de Steam en Program Files:
#     lee data/multiplayr.mod y escribe KenshiMP/kenshi-online.mod; probablemente ya no coinciden con la
#     instalación actual).
# EN: Renames the base mod "multiplayr.mod" to "kenshi-online.mod" inside the FCS data file: every
#     "XX-multiplayr.mod" string (with a u32 LE length prefix) becomes "XX-kenshi-online.mod" updating the
#     prefix (+3). Usage: python rebase_mod.py (hardcoded Steam paths under Program Files: reads
#     data/multiplayr.mod and writes KenshiMP/kenshi-online.mod; they probably no longer match the
#     current install).

"""
Rebase multiplayr.mod → kenshi-online.mod

FCS binary format: strings are 4-byte LE length-prefixed.
Every "multiplayr" occurrence is inside a string like "XX-multiplayr.mod"
with a length prefix of 17. We replace "multiplayr" with "kenshi-online",
growing each string from 17 to 20 chars, and update the length prefix.

We process occurrences left-to-right, copying unchanged bytes between them.
"""

import struct
import sys
from pathlib import Path

# ES: Texto a sustituir y diferencia de longitud.
# EN: Text to replace and length difference.
OLD = b"multiplayr"
NEW = b"kenshi-online"
DELTA = len(NEW) - len(OLD)  # +3

# ES: Offset del prefijo de longitud de la cadena que contiene string_start, o -1 si no cuadra.
# EN: Offset of the length prefix of the string containing string_start, or -1 if it does not match.
def find_length_prefix(data: bytes, string_start: int) -> int:
    """Walk back from the start of printable string to find where it begins,
    then check the 4 bytes before it for a LE uint32 length prefix."""
    s = string_start
    while s > 0 and 0x20 <= data[s - 1] < 0x7F:
        s -= 1
    if s < 4:
        return -1
    prefix_val = struct.unpack_from("<I", data, s - 4)[0]
    # ES: Buscar el final de la cadena imprimible
    # Find end of printable string
    e = string_start + len(OLD)
    while e < len(data) and 0x20 <= data[e] < 0x7F:
        e += 1
    actual_len = e - s
    # ES: El prefijo debe coincidir con la longitud (se admite +-1 por bytes finales)
    # The prefix should match the string length (allow off-by-one from trailing bytes)
    if abs(prefix_val - actual_len) <= 1 and prefix_val > 0:
        return s - 4  # offset of the length prefix
    return -1


# ES: Aplica todas las sustituciones y escribe el resultado en output_path.
# EN: Applies every replacement and writes the result to output_path.
def rebase(input_path: str, output_path: str):
    data = open(input_path, "rb").read()
    print(f"Input: {input_path} ({len(data)} bytes)")

    # ES: Buscar todas las apariciones
    # Find all occurrences
    positions = []
    start = 0
    while True:
        pos = data.find(OLD, start)
        if pos == -1:
            break
        positions.append(pos)
        start = pos + 1

    print(f"Found {len(positions)} occurrences of '{OLD.decode()}'")

    # ES: Plan de sustituciones: desde el prefijo de longitud hasta el final de la cadena
    # Build replacement plan: list of (prefix_offset, prefix_len_offset, old_string_region, new_string)
    # We need to replace from the length prefix through the end of the string
    replacements = []  # (region_start, region_end, new_bytes)

    for p in positions:
        prefix_off = find_length_prefix(data, p)

        if prefix_off >= 0:
            old_prefix_val = struct.unpack_from("<I", data, prefix_off)[0]
            new_prefix_val = old_prefix_val + DELTA
            string_start = prefix_off + 4
            string_end = string_start + old_prefix_val

            # ES: Nueva región: nuevo prefijo + cadena sustituida
            # Build new region: new length prefix + string with replacement
            old_string = data[string_start:string_end]
            new_string = old_string.replace(OLD, NEW, 1)
            new_region = struct.pack("<I", new_prefix_val) + new_string

            replacements.append((prefix_off, string_end, new_region))
        else:
            # ES: Alternativa: sustituir solo el texto en su sitio (no debería pasar)
            # Fallback: just replace the needle in-place (shouldn't happen)
            replacements.append((p, p + len(OLD), NEW))

    # ES: Eliminar regiones solapadas (algunas cadenas pueden compartir prefijo), ordenando por inicio
    # Deduplicate overlapping regions (some strings may share the same prefix)
    # Sort by start offset
    replacements.sort(key=lambda x: x[0])

    # ES: Fusionar sustituciones solapadas
    # Merge overlapping replacements
    merged = []
    for r in replacements:
        if merged and r[0] < merged[-1][1]:
            # ES: Solapada: se salta (ya la cubre la primera sustitución de ese rango)
            # Overlapping - skip (already handled by the first replacement covering this range)
            # But verify the replacement is consistent
            continue
        merged.append(r)

    print(f"Applying {len(merged)} replacements")

    # ES: Construir la salida
    # Build output
    out = bytearray()
    cursor = 0
    for region_start, region_end, new_bytes in merged:
        out.extend(data[cursor:region_start])
        out.extend(new_bytes)
        cursor = region_end

    out.extend(data[cursor:])

    # ES: Comprobar que no queda el nombre antiguo
    # Verify
    result = bytes(out)
    remaining = result.find(OLD)
    if remaining != -1:
        print(f"WARNING: '{OLD.decode()}' still found at offset 0x{remaining:04X}!")
    else:
        print(f"All occurrences replaced successfully")

    # ES: Comprobar que aparece el nombre nuevo
    # Verify new name appears
    count = 0
    s = 0
    while True:
        pos = result.find(NEW, s)
        if pos == -1:
            break
        count += 1
        s = pos + 1
    print(f"'{NEW.decode()}' appears {count} times in output")

    print(f"Output: {output_path} ({len(result)} bytes, delta={len(result)-len(data):+d})")
    open(output_path, "wb").write(result)
    print("Done!")


# ES: Punto de entrada con rutas fijas.
# EN: Entry point with hardcoded paths.
if __name__ == "__main__":
    kenshi_dir = r"C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
    input_mod = f"{kenshi_dir}/data/multiplayr.mod"
    output_mod = f"{kenshi_dir}/KenshiMP/kenshi-online.mod"
    rebase(input_mod, output_mod)
