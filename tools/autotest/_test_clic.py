# Test puntual: enfocar Kenshi y clicar CONTINUAR con pyautogui (lo que usa el autotest)
import pyautogui, pygetwindow as gw, time
pyautogui.FAILSAFE = False

# Buscar la ventana del JUEGO (ancha, no el server)
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
    x = w.left + int(w.width * 0.40)
    y = w.top + int(w.height * 0.404)
    print(f"Clic MULTIPLAYER pyautogui en ({x},{y})")
    pyautogui.moveTo(x, y, duration=0.4)
    pyautogui.click()
    time.sleep(1)
    print("Clic enviado.")
