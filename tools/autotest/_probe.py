# Sonda temporal para probar clics en el panel MP del mod (JOIN GAME).
import pyautogui, pygetwindow as gw, time, sys

# Localizar la ventana del juego (la mas ancha, excluyendo el server).
ws = [w for w in gw.getWindowsWithTitle("Kenshi") if "Server" not in w.title and ".exe" not in w.title.lower()]
w = max(ws, key=lambda v: v.width)
w.activate(); time.sleep(0.5)

accion = sys.argv[1] if len(sys.argv) > 1 else "join"
# Coordenadas relativas de los botones del panel MP del mod (medidas a ojo).
botones = {
    "host":   (0.575, 0.22),
    "join":   (0.575, 0.31),
    "browser":(0.575, 0.49),
}
rx, ry = botones.get(accion, botones["join"])
x = w.left + int(w.width*rx); y = w.top + int(w.height*ry)
pyautogui.moveTo(x, y, duration=0.5); time.sleep(0.5)
pyautogui.click()
print(f"clic {accion} en ({x},{y})")
