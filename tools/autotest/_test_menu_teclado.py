# ES: Prueba si el menú nativo de Kenshi responde a teclado sintético (el ratón no funcionaba): cierra el
#     panel MP del mod (F1) y Esc, y luego envía arriba + Enter + Espacio. Uso: python _test_menu_teclado.py
# EN: Tests whether Kenshi's native menu responds to synthetic keyboard input (the mouse did not work):
#     closes the mod's MP panel (F1) and Esc, then sends up + Enter + Space. Usage: python _test_menu_teclado.py

# ¿El menu NATIVO de Kenshi responde a teclado? (el raton no funciona)
# Probamos secuencias tipicas de navegacion de menu.
# EN: Does Kenshi's NATIVE menu respond to the keyboard? (the mouse does not work)
#     We try typical menu navigation sequences.
import time, pyautogui, pygetwindow as gw
pyautogui.FAILSAFE = False

wins = [w for w in gw.getWindowsWithTitle("Kenshi")
        if "Server" not in (w.title or "") and ".exe" not in (w.title or "").lower()
        and w.width > 800]
if not wins:
    print("No hay ventana de juego"); raise SystemExit
w = wins[0]
try:
    w.activate(); time.sleep(0.8)
except Exception as e:
    print("activate fallo:", e)

# Primero cerrar el panel MP del mod si esta abierto (F1 toggle) y Esc
# EN: First close the mod's MP panel if open (F1 toggle) and Esc
pyautogui.press("f1"); time.sleep(0.5)
pyautogui.press("esc"); time.sleep(0.8)

# Intento A: flechas + Enter (navegacion estandar)
# EN: Attempt A: arrows + Enter (standard navigation); the code sends Up, not Down
print("A: Down x1 + Enter")
pyautogui.press("up"); time.sleep(0.3)   # subir al primero (CONTINUAR)
pyautogui.press("enter"); time.sleep(0.3)
pyautogui.press("space"); time.sleep(0.3)
print("Secuencia teclado enviada.")
