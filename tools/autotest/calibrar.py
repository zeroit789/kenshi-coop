#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ES: Calibrador de coordenadas del autotest: cada 0.5 s imprime la posición del ratón en píxeles y en
#     coordenadas relativas (0..1) a la ventana de Kenshi, para copiar rel_x/rel_y a config.json.
#     Con --shot guarda además debug_screenshot.png. Uso: python calibrar.py [--shot] (Ctrl+C para salir).
# EN: Autotest coordinate calibrator: every 0.5 s prints the mouse position in pixels and in coordinates
#     relative (0..1) to the Kenshi window, so rel_x/rel_y can be copied into config.json.
#     With --shot it also saves debug_screenshot.png. Usage: python calibrar.py [--shot] (Ctrl+C to quit).

"""
calibrar.py
===========
Herramienta de CALIBRACION para el autotest. La automatizacion de GUI es fragil:
si los clics caen en el sitio equivocado, usa esto para averiguar las coordenadas
reales de los botones del menu de Kenshi en TU pantalla/resolucion.

USO:
  1) Arranca Kenshi a mano hasta el MENU PRINCIPAL (o donde quieras medir).
  2) python calibrar.py
  3) Mueve el raton encima del boton que te interese (ej: CONTINUE).
  4) El script imprime cada 0.5s la posicion del raton en pixel Y en
     coordenadas relativas (0..1) respecto a la ventana de Kenshi.
  5) Copia los valores 'rel_x'/'rel_y' a config.json si hace falta corregir.

  python calibrar.py --shot     # ademas guarda un screenshot de la pantalla
                                  # (debug_screenshot.png) para inspeccion visual.

Ctrl+C para salir.
"""

import argparse
import time
import sys
from pathlib import Path

# ES: Dependencias de automatización de GUI; sin ellas el script no puede funcionar.
# EN: GUI automation dependencies; the script cannot work without them.
try:
    import pyautogui
    import pygetwindow as gw
except ImportError as e:
    print(f"[FATAL] Falta dependencia: {e}")
    print("        python -m pip install pyautogui pygetwindow")
    sys.exit(2)

SCRIPT_DIR = Path(__file__).resolve().parent


# ES: Devuelve la primera ventana cuyo título contiene "Kenshi" (o None).
# EN: Returns the first window whose title contains "Kenshi" (or None).
def encontrar_ventana_kenshi():
    """Localiza la ventana de Kenshi por titulo."""
    for titulo in ("Kenshi", "kenshi"):
        v = gw.getWindowsWithTitle(titulo)
        if v:
            return v[0]
    return None


# ES: Punto de entrada: argumentos, búsqueda de la ventana, captura opcional y bucle de medición.
# EN: Entry point: arguments, window lookup, optional screenshot and measuring loop.
def main():
    parser = argparse.ArgumentParser(description="Calibrador de coordenadas para autotest Kenshi.")
    parser.add_argument("--shot", action="store_true",
                        help="Guardar tambien un screenshot debug_screenshot.png")
    args = parser.parse_args()

    win = encontrar_ventana_kenshi()
    if win is None:
        print("[AVISO] No encuentro la ventana de Kenshi. Mostrare solo pixeles de pantalla.")
    else:
        print(f"[OK] Ventana Kenshi: pos=({win.left},{win.top}) size=({win.width}x{win.height})")

    if args.shot:
        ruta = SCRIPT_DIR / "debug_screenshot.png"
        pyautogui.screenshot(str(ruta))
        print(f"[OK] Screenshot guardado en {ruta}")

    print("\nMueve el raton sobre el boton a medir. Ctrl+C para salir.\n")
    # ES: Bucle de medición hasta Ctrl+C; sin ventana usa coordenadas relativas a la pantalla.
    # EN: Measuring loop until Ctrl+C; without a window it uses screen-relative coordinates.
    try:
        while True:
            px, py = pyautogui.position()
            if win is not None and win.width > 0 and win.height > 0:
                rx = (px - win.left) / win.width
                ry = (py - win.top) / win.height
                print(f"  pixel=({px:>4},{py:>4})   rel=({rx:.4f}, {ry:.4f})", end="\r", flush=True)
            else:
                sw, sh = pyautogui.size()
                print(f"  pixel=({px:>4},{py:>4})   rel_pantalla=({px/sw:.4f}, {py/sh:.4f})",
                      end="\r", flush=True)
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n\nCalibracion terminada.")


if __name__ == "__main__":
    main()
