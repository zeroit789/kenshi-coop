# ES: Prueba puntual: enfoca la ventana del juego (>800 px de ancho, no el servidor) y hace clic con
#     pyautogui en el botón MULTIPLAYER del menú principal (posición relativa 0.40 / 0.404).
#     Uso: python _test_clic.py
# EN: One-off test: focuses the game window (>800 px wide, not the server) and clicks with pyautogui on the
#     main-menu MULTIPLAYER button (relative position 0.40 / 0.404). Usage: python _test_clic.py

# Test puntual: enfocar Kenshi y clicar CONTINUAR con pyautogui (lo que usa el autotest)
# EN: One-off test: focus Kenshi and click CONTINUE with pyautogui (what the autotest uses)
#     (the code actually clicks MULTIPLAYER)
import pyautogui, pygetwindow as gw, time
pyautogui.FAILSAFE = False

# Buscar la ventana del JUEGO (ancha, no el server)
# EN: Find the GAME window (wide, not the server)
wins = [w for w in gw.getWindowsWithTitle("Kenshi")
        if "Server" not in (w.title or "") and ".exe" not in (w.title or "").lower()]
wins = [w for w in wins if w.width > 800]

if not wins:
    print("No hay ventana de juego (>800px)")
else:
    w = wins[0]
    try:
        w.activate(); time.sleep(0.8)
    except Exception as e:
        print("activate fallo:", e)
    print(f"Ventana: {w.left},{w.top} {w.width}x{w.height}")
    # MULTIPLAYER centro real medido: rel 0.40 horizontal, 0.404 vertical
    # EN: real measured MULTIPLAYER center: rel 0.40 horizontal, 0.404 vertical
    x = w.left + int(w.width * 0.40)
    y = w.top + int(w.height * 0.404)
    print(f"Clic MULTIPLAYER pyautogui en ({x},{y})")
    pyautogui.moveTo(x, y, duration=0.4)
    pyautogui.click()
    time.sleep(1)
    print("Clic enviado.")
