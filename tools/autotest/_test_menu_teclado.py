# ¿El menu NATIVO de Kenshi responde a teclado? (el raton no funciona)
# Probamos secuencias tipicas de navegacion de menu.
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
pyautogui.press("f1"); time.sleep(0.5)
pyautogui.press("esc"); time.sleep(0.8)

# Intento A: flechas + Enter (navegacion estandar)
print("A: Down x1 + Enter")
pyautogui.press("up"); time.sleep(0.3)   # subir al primero (CONTINUAR)
pyautogui.press("enter"); time.sleep(0.3)
pyautogui.press("space"); time.sleep(0.3)
print("Secuencia teclado enviada.")
