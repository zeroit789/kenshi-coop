# Test: ¿Kenshi/el mod captan input de TECLADO sintetico?
# El mod instala un WndProc hook (render_hooks), que podria recibir mensajes de
# teclado aunque OIS ignore el raton. Probamos F1 (menu MP del mod) y la consola.
import time
import pyautogui
import pygetwindow as gw
pyautogui.FAILSAFE = False

wins = [w for w in gw.getWindowsWithTitle("Kenshi")
        if "Server" not in (w.title or "") and ".exe" not in (w.title or "").lower()
        and w.width > 800]
if not wins:
    print("No hay ventana de juego")
    raise SystemExit

w = wins[0]
try:
    w.activate(); time.sleep(0.8)
except Exception as e:
    print("activate fallo:", e)
print(f"Ventana: {w.left},{w.top} {w.width}x{w.height}")

# Probar F1 (deberia abrir el menu MP del mod segun README)
print("Pulsando F1 (menu MP del mod)...")
pyautogui.press("f1")
time.sleep(2)

# Probar tambien la tecla backtick (debug HUD) e Insert (panel log)
print("Pulsando Insert (panel log)...")
pyautogui.press("insert")
time.sleep(1)
print("Teclas enviadas.")
