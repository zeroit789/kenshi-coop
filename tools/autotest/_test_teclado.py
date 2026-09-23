# ES: Prueba si Kenshi / el mod reciben teclado sintético: enfoca la ventana del juego y pulsa F1 (menú MP
#     del mod) e Insert (panel de log). El mod instala un hook de WndProc que podría ver estas teclas
#     aunque OIS ignore el ratón. Uso: python _test_teclado.py
# EN: Tests whether Kenshi / the mod receive synthetic keyboard input: focuses the game window and presses
#     F1 (mod MP menu) and Insert (log panel). The mod installs a WndProc hook that might see these keys
#     even if OIS ignores the mouse. Usage: python _test_teclado.py

# Test: ¿Kenshi/el mod captan input de TECLADO sintetico?
# El mod instala un WndProc hook (render_hooks), que podria recibir mensajes de
# teclado aunque OIS ignore el raton. Probamos F1 (menu MP del mod) y la consola.
# EN: Test: do Kenshi/the mod catch synthetic KEYBOARD input?
#     The mod installs a WndProc hook (render_hooks), which could receive keyboard
#     messages even if OIS ignores the mouse. We try F1 (mod MP menu) and the console.
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
# EN: Try F1 (should open the mod MP menu according to the README)
print("Pulsando F1 (menu MP del mod)...")
pyautogui.press("f1")
time.sleep(2)

# Probar tambien la tecla backtick (debug HUD) e Insert (panel log)
# EN: Also try the backtick key (debug HUD) and Insert (log panel); only Insert is sent
print("Pulsando Insert (panel log)...")
pyautogui.press("insert")
time.sleep(1)
print("Teclas enviadas.")
