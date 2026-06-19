# Test definitivo: clic via SendInput (API moderna) sobre MULTIPLAYER.
# Si esto tampoco saca a Kenshi del menu, el juego ignora input sintetico.
import ctypes, time
from ctypes import wintypes

user32 = ctypes.windll.user32

# Estructuras para SendInput
PUL = ctypes.POINTER(ctypes.c_ulong)
class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", PUL)]
class INPUT(ctypes.Structure):
    class _I(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT)]
    _anonymous_ = ("i",)
    _fields_ = [("type", wintypes.DWORD), ("i", _I)]

INPUT_MOUSE = 0
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

# Resolucion virtual para coords absolutas (0..65535)
SM_CXSCREEN, SM_CYSCREEN = 0, 1
sw = user32.GetSystemMetrics(SM_CXSCREEN)
sh = user32.GetSystemMetrics(SM_CYSCREEN)

def send_click_abs(px, py):
    ax = int(px * 65535 / sw)
    ay = int(py * 65535 / sh)
    extra = ctypes.c_ulong(0)
    for flags in (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                  MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE,
                  MOUSEEVENTF_LEFTUP | MOUSEEVENTF_ABSOLUTE):
        mi = MOUSEINPUT(ax, ay, 0, flags, 0, ctypes.pointer(extra))
        inp = INPUT(INPUT_MOUSE, INPUT._I(mi))
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.05)

# MULTIPLAYER en (768, 436)
print(f"SendInput clic en (768,436) [pantalla {sw}x{sh}]")
send_click_abs(768, 436)
time.sleep(1)
print("SendInput enviado.")
