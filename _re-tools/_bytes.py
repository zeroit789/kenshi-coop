# ES: Script de ingeniería inversa: vuelca en hexadecimal los 28 primeros bytes de la función
#     del juego en el RVA 0x7874E0 de kenshi_x64.exe (RVA = dirección relativa a la base del módulo).
#     Uso: python _bytes.py (ruta de Steam fija). Sirve para ver el prólogo y sacar una firma AOB.
# EN: Reverse-engineering script: dumps in hex the first 28 bytes of the game function at
#     RVA 0x7874E0 of kenshi_x64.exe (RVA = address relative to the module base).
#     Usage: python _bytes.py (hardcoded Steam path). Used to inspect the prologue and build an AOB signature.

import pefile
pe=pefile.PE(r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe",fast_load=True)
print(pe.get_data(0x7874E0,28).hex(' '))
