#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ES: Autotest del mod co-op (v2, guiado por log): automatiza el ciclo de prueba completo sin que nadie
#     juegue a mano. Mata Kenshi, asegura el servidor KenshiMP.Server.exe, fuerza en settings.cfg el save
#     que carga CONTINUE, lanza kenshi_x64.exe, pulsa OK en el launcher de configuración, recoloca la
#     ventana (evita que el render se pare fuera de pantalla), y avanza leyendo en vivo el log
#     KenshiOnline_<PID>.log (phase=MainMenu, gameLoaded=true, connected=true) para clicar MULTIPLAYER y
#     CONTINUE, conectar (o /connect) y opcionalmente atacar. Al final resume el log (fases y DIAG-*).
#     Configuración en config.json. Uso: python autotest_kenshi.py [--no-attack] [--dry-run]
#     [--keep-open] [--no-server]. Nota: la docstring y un print contienen texto que no se cambia aquí
#     porque es código (cadenas), no comentarios.
# EN: Co-op mod autotest (v2, log-driven): automates the whole test cycle without anyone playing by hand.
#     Kills Kenshi, makes sure the KenshiMP.Server.exe server is running, forces in settings.cfg the save
#     that CONTINUE loads, launches kenshi_x64.exe, presses OK on the config launcher, repositions the
#     window (prevents the renderer from stalling off-screen), and advances by reading the live log
#     KenshiOnline_<PID>.log (phase=MainMenu, gameLoaded=true, connected=true) to click MULTIPLAYER and
#     CONTINUE, connect (or /connect) and optionally attack. At the end it summarizes the log (phases and
#     DIAG-*). Configuration in config.json. Usage: python autotest_kenshi.py [--no-attack] [--dry-run]
#     [--keep-open] [--no-server]. Note: the docstring and one print contain text that is not changed
#     here because it is code (strings), not comments.

"""
autotest_kenshi.py  (v2 — guiado por log)
=========================================
Automatiza el CICLO DE PRUEBA del mod co-op de Kenshi para que Onyx
pueda probar el mod SIN que un humano juegue a mano.

CAMBIO CLAVE v2 (fiabilidad):
  La v1 avanzaba con esperas CIEGAS (time.sleep grandes). Eso era fragil: si
  Kenshi tardaba mas o menos de lo previsto, los clics caian en pantallas
  equivocadas. La v2 LEE EL LOG EN VIVO (KenshiOnline_<PID>.log) y avanza en
  cuanto detecta cada fase escrita por render_hooks.cpp:
      phase=MainMenu  gameLoaded=false  -> estoy en el menu (clicar aqui)
      phase=Loading                     -> cargando save
      phase=Connecting gameLoaded=true  -> YA ENTRE AL MUNDO
      phase=Connected  connected=true   -> conectado al server (exito total)
  Asi el script no clica hasta que el menu esta REALMENTE delante, ni declara
  exito hasta que el log lo confirma. Las esperas pasan a ser solo TIMEOUTS.

QUE HACE (paso a paso):
  1. Mata instancias previas de Kenshi (NO el server: la tarea pide dejarlo).
  2. Lanza KenshiMP.Server.exe si no esta ya corriendo.
  3. Fuerza en settings.cfg que CONTINUE cargue un save concreto (salta la
     creacion de personaje, lo mas fragil de automatizar).
  4. Lanza kenshi_x64.exe (el mod se carga solo como plugin de Ogre).
  5. ESPERA POR LOG a phase=MainMenu, luego clica MULTIPLAYER + CONTINUE.
  6. ESPERA POR LOG a gameLoaded=true (entro al mundo).
  7. ESPERA POR LOG a connected=true (conectado). Si no, manda /connect.
  8. (Opcional) intenta atacar a un NPC.
  9. Resume el log: fases alcanzadas + DIAG-PAUSE / DIAG-FAC + senales OK/fallo.

POR QUE ESTA VIA:
  - kenshi_x64.exe NO acepta args CLI para cargar saves (verificado a nivel de
    binario: ni importa GetCommandLineW). Por eso no se puede saltar el menu.
  - El sistema "Continue" (settings.cfg -> continue=<save>) SI carga un save de
    un clic, saltandose la creacion de personaje.
  - El mod se carga como plugin (Plugins_x64.cfg): lanzar el exe ya lo carga.

USO:
  python autotest_kenshi.py                # ciclo completo
  python autotest_kenshi.py --no-attack    # entra al mundo pero no ataca
  python autotest_kenshi.py --dry-run      # solo muestra el plan, no lanza nada
  python autotest_kenshi.py --keep-open    # no mata Kenshi al terminar
  python autotest_kenshi.py --no-server    # no toca el server (asume ya corriendo)
"""

import argparse
import ctypes
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

# ---- Dependencias de automatizacion GUI ----
# EN: ---- GUI automation dependencies ----
try:
    import pyautogui
    import pygetwindow as gw
except ImportError as e:
    print(f"[FATAL] Falta una dependencia: {e}")
    print("        Instala con: python -m pip install pyautogui pygetwindow")
    sys.exit(2)

# pyautogui: fail-safe = mover el raton a la esquina sup-izq aborta. Lo dejamos ON
# como seguridad, pero metemos pausas para que no vaya disparado.
# EN: pyautogui: fail-safe = moving the mouse to the top-left corner aborts. We keep it ON
#     as a safety net, but add pauses so it does not run too fast.
pyautogui.FAILSAFE = True
pyautogui.PAUSE = 0.3  # pausa entre acciones de pyautogui

# Ruta de este script (para encontrar config.json al lado)
# EN: Path of this script (to find config.json next to it)
SCRIPT_DIR = Path(__file__).resolve().parent
CONFIG_PATH = SCRIPT_DIR / "config.json"


# ======================================================================
# Utilidades de log de consola (lo que se ve por stdout)
# ======================================================================
# EN: Console log utilities (what is seen on stdout)
# ES: Imprime un mensaje con marca de hora (hh:mm:ss).
# EN: Prints a message with a timestamp (hh:mm:ss).
def log(msg):
    """Imprime con timestamp para que Onyx siga el flujo."""
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ES: Lee config.json (junto al script); aborta si no existe.
# EN: Reads config.json (next to the script); aborts if missing.
def cargar_config():
    """Lee config.json. Aborta si no existe o esta mal formado."""
    if not CONFIG_PATH.exists():
        log(f"[FATAL] No existe config.json en {CONFIG_PATH}")
        sys.exit(2)
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


# ======================================================================
# Gestion de procesos (matar / lanzar)
# ======================================================================
# EN: Process management (kill / launch)
# ES: Mata solo kenshi_x64.exe; el servidor se deja vivo.
# EN: Kills only kenshi_x64.exe; the server is left running.
def matar_kenshi():
    """Mata SOLO Kenshi (no el server). La tarea pide dejar el server vivo."""
    subprocess.run(["taskkill", "/F", "/IM", "kenshi_x64.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log("Instancia previa de Kenshi terminada (si la habia). El server NO se toca.")


# ES: True si KenshiMP.Server.exe aparece en tasklist.
# EN: True if KenshiMP.Server.exe shows up in tasklist.
def server_corriendo():
    """Devuelve True si KenshiMP.Server.exe esta en ejecucion."""
    # tasklist filtra por nombre de imagen; si aparece la linea, esta vivo.
    # EN: tasklist filters by image name; if the line shows up, it is alive.
    out = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq KenshiMP.Server.exe", "/NH"],
        capture_output=True, text=True
    )
    return "KenshiMP.Server.exe" in out.stdout


# ES: Lanza el servidor en una consola propia si no está corriendo (y está permitido); devuelve el proceso o None.
# EN: Starts the server in its own console if it is not running (and allowed); returns the process or None.
def asegurar_server(cfg, tocar_server):
    """
    Si el server ya corre, no hace nada (lo deja como pide la tarea).
    Si no corre y tocar_server=True, lo lanza. Devuelve el proc o None.
    """
    if server_corriendo():
        log("Server MP ya esta corriendo. Lo dejo tal cual (no lo toco).")
        return None
    if not tocar_server:
        log("[AVISO] El server NO esta corriendo y --no-server impide lanzarlo. "
            "El auto-connect fallara. Lanza KenshiMP.Server.exe a mano.")
        return None
    server = cfg["rutas"]["server_exe"]
    game_dir = cfg["rutas"]["game_dir"]
    if not os.path.exists(server):
        log(f"[FATAL] No existe el server: {server}")
        sys.exit(2)
    # cwd = game_dir para que encuentre server.json. Ventana propia para no bloquear.
    # EN: cwd = game_dir so it finds server.json. Own window so it does not block.
    proc = subprocess.Popen([server], cwd=game_dir,
                            creationflags=subprocess.CREATE_NEW_CONSOLE)
    log(f"Server MP lanzado (PID {proc.pid}) en 127.0.0.1:{cfg['red']['server_port']}.")
    time.sleep(cfg["tiempos_segundos"]["espera_server_arranque"])
    return proc


# ES: Lanza kenshi_x64.exe desde la carpeta del juego; el mod se carga solo como plugin de Ogre.
# EN: Launches kenshi_x64.exe from the game folder; the mod loads itself as an Ogre plugin.
def lanzar_kenshi(cfg):
    """Arranca kenshi_x64.exe. El mod se carga solo (plugin de Ogre)."""
    kenshi = cfg["rutas"]["kenshi_exe"]
    game_dir = cfg["rutas"]["game_dir"]
    if not os.path.exists(kenshi):
        log(f"[FATAL] No existe Kenshi: {kenshi}")
        sys.exit(2)
    proc = subprocess.Popen([kenshi], cwd=game_dir)
    log(f"Kenshi lanzado (PID {proc.pid}). El mod KenshiMP.Core se carga como plugin.")
    return proc


# ======================================================================
# Preparar el save que cargara CONTINUE
# ======================================================================
# EN: Prepare the save that CONTINUE will load
# ES: Escribe continue=<save> en settings.cfg para que CONTINUE cargue ese save (salta la creación de personaje).
# EN: Writes continue=<save> into settings.cfg so CONTINUE loads that save (skips character creation).
def forzar_continue_save(cfg):
    """
    Reescribe la clave 'continue=<save>' en settings.cfg para que CONTINUE
    cargue el save que queremos, saltandose la creacion de personaje.
    """
    settings = Path(cfg["rutas"]["settings_cfg"])
    save = cfg["save"]["continue_save"]
    save_dir = Path(cfg["rutas"]["save_dir"]) / save

    if not save_dir.exists():
        log(f"[AVISO] El save '{save}' no existe en {save_dir}. "
            f"CONTINUE podria fallar. Saves disponibles: "
            f"{[p.name for p in Path(cfg['rutas']['save_dir']).iterdir() if p.is_dir()]}")

    if not settings.exists():
        log(f"[AVISO] No existe settings.cfg en {settings}; no puedo forzar continue.")
        return

    # ES: Sustituye (o añade) la línea continue= conservando el resto del fichero.
    # EN: Replaces (or appends) the continue= line keeping the rest of the file.
    lineas = settings.read_text(encoding="utf-8", errors="ignore").splitlines()
    nuevas, encontrada = [], False
    for ln in lineas:
        if ln.startswith("continue="):
            nuevas.append(f"continue={save}")
            encontrada = True
        else:
            nuevas.append(ln)
    if not encontrada:
        nuevas.append(f"continue={save}")
    settings.write_text("\n".join(nuevas) + "\n", encoding="utf-8")
    log(f"settings.cfg -> continue={save} (CONTINUE cargara este save).")


# ======================================================================
# Localizacion y lectura EN VIVO del log
# ======================================================================
# EN: Locating and reading the log LIVE
# ES: Espera a que aparezca un KenshiOnline_<PID>.log nuevo tras lanzar Kenshi y devuelve su ruta (o None).
# EN: Waits for a new KenshiOnline_<PID>.log to appear after launching Kenshi and returns its path (or None).
def esperar_log_nuevo(cfg, desde_ts, timeout=30):
    """
    Tras lanzar Kenshi, espera a que aparezca un KenshiOnline_<PID>.log nuevo
    (creado despues de desde_ts). Devuelve la ruta o None si no aparece.
    Ignora KenshiOnline_Server.log y KenshiOnline_CRASH.log.
    """
    game_dir = Path(cfg["rutas"]["game_dir"])
    t0 = time.time()
    while time.time() - t0 < timeout:
        candidatos = [
            p for p in game_dir.glob("KenshiOnline_*.log")
            if p.stat().st_mtime >= desde_ts - 2
            and re.match(r"KenshiOnline_\d+\.log$", p.name)  # solo PID numerico
        ]
        if candidatos:
            ruta = max(candidatos, key=lambda p: p.stat().st_mtime)
            log(f"Log de esta sesion: {ruta.name}")
            return ruta
        time.sleep(1)
    log("[AVISO] No aparecio un log de sesion nuevo en el tiempo esperado.")
    return None


# ES: Lee el log de forma incremental hasta que una línea casa con el patrón (regex) o vence el timeout.
# EN: Reads the log incrementally until a line matches the pattern (regex) or the timeout expires.
def esperar_patron_en_log(ruta, patron, timeout, poll, etiqueta):
    """
    Lee el log EN VIVO y espera a que aparezca 'patron' (regex). Devuelve la
    primera linea que casa, o None si se agota el timeout.
    Lee el archivo completo cada poll (los logs son grandes pero esto es robusto
    frente a reaperturas; solo nos importa que aparezca el patron).
    """
    if ruta is None:
        return None
    rx = re.compile(patron)
    t0 = time.time()
    ultima_pos = 0
    log(f"Esperando en el log: {etiqueta}  (patron: {patron}, timeout {timeout}s)...")
    while time.time() - t0 < timeout:
        try:
            with open(ruta, "r", encoding="utf-8", errors="ignore") as f:
                f.seek(ultima_pos)
                trozo = f.read()
                ultima_pos = f.tell()
        except Exception:
            time.sleep(poll)
            continue
        for ln in trozo.splitlines():
            if rx.search(ln):
                log(f"  -> DETECTADO {etiqueta}: {ln.strip()[:120]}")
                return ln.strip()
        time.sleep(poll)
    log(f"  -> [TIMEOUT] No aparecio {etiqueta} en {timeout}s.")
    return None


# ======================================================================
# Recolocacion de ventana (CAUSA RAIZ del cuelgue en splash)
# ======================================================================
# EN: Window repositioning (ROOT CAUSE of the splash hang)
# ES: Mueve la ventana del juego a (0,0) 1920x1080 y al frente: si se lanza desde una sesión no
#     interactiva la ventana sale fuera de pantalla y el render de Kenshi no avanza.
# EN: Moves the game window to (0,0) 1920x1080 and to the front: when launched from a non-interactive
#     session the window opens off-screen and Kenshi's renderer does not progress.
def recolocar_ventana_kenshi(timeout=25):
    """
    *** ARREGLO CRITICO (verificado 2026-06-18) ***
    Cuando se lanza kenshi_x64.exe desde una sesion NO interactiva (agente /
    PowerShell sin escritorio en primer plano), Kenshi abre su ventana FUERA
    de la pantalla visible (ej. X=3276) y a tamano pequeno (488x572). Mientras
    la ventana esta off-screen, Windows NO la compone y el RENDER LOOP de Kenshi
    se QUEDA PARADO: no escribe ni un frame, nunca llega al menu, y acaba
    pareciendo un cuelgue. Esto NO pasa cuando un humano lo lanza (la ventana
    sale centrada y visible).

    La cura: en cuanto aparece la ventana, la movemos a (0,0) con tamano
    1920x1080 via Win32 SetWindowPos + la traemos al frente. En el instante en
    que pasa a ser visible, el render arranca y Kenshi progresa hasta el menu.

    Devuelve True si recoloco la ventana, False si no la encontro a tiempo.
    """
    # Constantes Win32
    # EN: Win32 constants
    SWP_NOZORDER = 0x0004
    SW_RESTORE = 9
    user32 = ctypes.windll.user32

    t0 = time.time()
    while time.time() - t0 < timeout:
        # Buscamos la ventana de Kenshi (la mas ancha = el juego, NO el server).
        # EN: Look for the Kenshi window (the widest = the game, NOT the server).
        win = encontrar_ventana_kenshi()
        # No recolocar si lo unico que hay es el launcher estrecho de config:
        # eso lo maneja manejar_launcher_config(); redimensionarlo a 1920 romperia
        # la deteccion del boton OK. Solo recolocamos la ventana grande del juego.
        # EN: Do not reposition if all there is is the narrow config launcher:
        #     manejar_launcher_config() handles it; resizing it to 1920 would break
        #     the OK button detection. We only reposition the big game window.
        if win is not None and win.width < 800:
            time.sleep(1.0)
            continue
        if win is not None:
            try:
                hwnd = win._hWnd  # pygetwindow expone el handle nativo
            except Exception:
                hwnd = None
            if hwnd:
                user32.ShowWindow(hwnd, SW_RESTORE)
                # Mover a 0,0 y forzar 1920x1080 (resolucion configurada del juego).
                # EN: Move to 0,0 and force 1920x1080 (configured game resolution).
                user32.SetWindowPos(hwnd, 0, 0, 0, 1920, 1080, SWP_NOZORDER)
                user32.SetForegroundWindow(hwnd)
                time.sleep(1.0)
                log("Ventana de Kenshi recolocada a (0,0) 1920x1080 y al frente. "
                    "Esto despierta el render (evita el cuelgue por ventana off-screen).")
                return True
        time.sleep(1.0)
    log("[AVISO] No pude recolocar la ventana de Kenshi (no aparecio a tiempo). "
        "Si arranco off-screen, el render podria no progresar.")
    return False


# ======================================================================
# Foco y geometria de la ventana de Kenshi
# ======================================================================
# EN: Kenshi window focus and geometry
# ES: Ventanas cuyo título contiene "Kenshi", excluyendo la consola del servidor (títulos con Server o .exe).
# EN: Windows whose title contains "Kenshi", excluding the server console (titles with Server or .exe).
def _ventanas_kenshi_validas():
    """
    Devuelve TODAS las ventanas cuyo titulo contiene 'Kenshi' EXCLUYENDO la
    consola del server.
    *** BUG HISTORICO (arreglado 2026-06-18) ***
    La consola del server se titula '...\\KenshiMP.Server.exe' y TAMBIEN casa con
    'Kenshi'. getWindowsWithTitle('Kenshi') la devolvia la primera, asi que el
    script enfocaba/medianla por error: en _run1.log se ven size=(488x572) y
    luego (1129x635), que NO eran el juego sino el server y/o el launcher. Por
    eso los clics caian mal. Aqui filtramos cualquier titulo con 'Server' o '.exe'.
    """
    vistas = {}
    for titulo in ("Kenshi", "kenshi"):
        for v in gw.getWindowsWithTitle(titulo):
            t = v.title or ""
            if "Server" in t or ".exe" in t.lower():
                continue  # es la consola del server, no el juego/launcher
            # Clave por geometria+titulo para no duplicar la misma ventana.
            # EN: Key by geometry+title to avoid duplicating the same window.
            vistas[(v.left, v.top, v.width, v.height, t)] = v
    return list(vistas.values())


# ES: Ventana del juego/launcher (no la del servidor); si hay varias, la más ancha.
# EN: Game/launcher window (not the server one); if several, the widest.
def encontrar_ventana_kenshi():
    """
    Busca la ventana del JUEGO/launcher de Kenshi (NO la del server).
    Si hay varias candidatas, prefiere la mas ANCHA: el juego ya cargado mide
    ~1920px de ancho; el launcher de config mide ~488px. Devuelve obj o None.
    """
    candidatas = _ventanas_kenshi_validas()
    if not candidatas:
        return None
    return max(candidatas, key=lambda v: v.width)


# ES: Ventana del launcher de configuración: la más estrecha (<= ancho_max_launcher), o None.
# EN: Config launcher window: the narrowest one (<= ancho_max_launcher), or None.
def encontrar_launcher_config(cfg):
    """
    Localiza la ventana del LAUNCHER de config (el dialogo pequeno con OK).
    Se distingue del juego por el ANCHO: launcher ~488px, juego ~1920px. Mismo
    titulo ('Kenshi 1.0.68 - x64 (Newland)'), por eso no se puede filtrar por
    nombre. Devuelve la ventana del launcher o None si no esta (o ya es el juego).
    """
    ancho_max = cfg["launcher_config"].get("ancho_max_launcher", 800)
    estrechas = [v for v in _ventanas_kenshi_validas() if 0 < v.width <= ancho_max]
    if not estrechas:
        return None
    # Si hay varias estrechas, la mas pequena es el dialogo de config.
    # EN: If there are several narrow ones, the smallest is the config dialog.
    return min(estrechas, key=lambda v: v.width)


# ES: Espera al launcher de configuración que sale al arrancar Kenshi y pulsa su OK (por imagen
#     boton_ok.png o por coordenadas relativas), con un reintento. Sin ese OK el juego no arranca.
# EN: Waits for the config launcher shown when Kenshi starts and presses its OK (by image boton_ok.png or
#     by relative coordinates), with one retry. Without that OK the game does not start.
def manejar_launcher_config(cfg):
    """
    *** PASO NUEVO (pista de Zero, verificado 2026-06-18) ***
    Al lanzar kenshi_x64.exe SIEMPRE aparece primero el LAUNCHER de config: un
    dialogo pequeno (~488x572) con Direct3D 11 / Resolucion 1920x1080 / RTX 4070
    Ti / Idioma Espanol y un boton OK abajo a la derecha. El render loop del
    juego (y por tanto phase=MainMenu en el log) NO arranca hasta pulsar ese OK.
    El intento anterior del autotest se quedaba ATASCADO aqui porque no lo
    manejaba (timeout de menu expiraba esperando un menu que no podia llegar).

    Estrategia:
      1) Esperar a que aparezca la ventana estrecha del launcher (timeout config).
      2) Clic en OK: primero por IMAGEN (boton_ok.png con locateOnScreen, robusto
         a que la ventana se mueva), y si no se encuentra, por COORDS RELATIVAS
         dentro de la ventana del launcher (ok_rel_x/ok_rel_y).
      3) Esperar unos segundos a que el juego empiece a cargar de verdad.

    Devuelve True si pulso OK (o no hubo launcher que manejar), False si el
    launcher apareció pero no se pudo pulsar OK.
    """
    lc = cfg["launcher_config"]
    if not lc.get("habilitado", True):
        log("Manejo de launcher de config DESACTIVADO en config. Salto este paso.")
        return True

    timeout = lc.get("timeout_aparece_launcher", 30)
    plantilla = SCRIPT_DIR / lc.get("plantilla_ok", "boton_ok.png")
    confianza = lc.get("confianza_match", 0.8)

    # 1) Esperar a que el launcher aparezca.
    # EN: 1) Wait for the launcher to appear.
    log(f"Esperando la ventana de CONFIGURACION inicial (launcher) "
        f"(timeout {timeout}s)...")
    t0 = time.time()
    win_launcher = None
    while time.time() - t0 < timeout:
        win_launcher = encontrar_launcher_config(cfg)
        if win_launcher is not None:
            log(f"  -> Launcher de config DETECTADO: pos=({win_launcher.left},"
                f"{win_launcher.top}) size=({win_launcher.width}x{win_launcher.height}).")
            break
        time.sleep(1.0)

    if win_launcher is None:
        # No siempre sale (p.ej. si ya esta cargando). No es error fatal: puede
        # que el juego pase directo. Avisamos y seguimos al flujo de menu.
        # EN: It does not always show up (e.g. if it is already loading). Not a fatal error: the game
        #     may go straight through. We warn and continue with the menu flow.
        log("[AVISO] No apareció la ventana de config en el tiempo esperado. "
            "Quizá Kenshi pasó directo a cargar. Sigo al flujo de menu.")
        return True

    # Traer el launcher al frente para que el clic le llegue.
    # EN: Bring the launcher to the front so the click reaches it.
    try:
        if win_launcher.isMinimized:
            win_launcher.restore()
        win_launcher.activate()
        time.sleep(0.5)
    except Exception as e:
        log(f"[AVISO] No pude activar el launcher: {e}. Intento clicar igual.")

    # 2a) Intento por IMAGEN (preferido: robusto a la posicion).
    # EN: 2a) Attempt by IMAGE (preferred: robust to position).
    ok_clicado = False
    if plantilla.exists():
        try:
            loc = pyautogui.locateOnScreen(str(plantilla), confidence=confianza)
        except Exception as e:
            # locateOnScreen lanza si no encuentra (segun version) o si falta opencv.
            # EN: locateOnScreen raises if it finds nothing (depending on version) or if opencv is missing.
            log(f"[AVISO] locateOnScreen fallo o no encontro el OK por imagen: {e}")
            loc = None
        if loc is not None:
            cx, cy = pyautogui.center(loc)
            log(f"Boton OK localizado por IMAGEN en ({cx},{cy}). Clic.")
            pyautogui.moveTo(cx, cy, duration=0.3)
            pyautogui.click()
            ok_clicado = True
    else:
        log(f"[AVISO] No existe la plantilla {plantilla.name}. Uso coords relativas.")

    # 2b) Fallback por COORDS RELATIVAS dentro de la ventana del launcher.
    # EN: 2b) Fallback by RELATIVE COORDS inside the launcher window.
    if not ok_clicado:
        # Re-leer la geometria por si se movio.
        # EN: Re-read the geometry in case it moved.
        win_launcher = encontrar_launcher_config(cfg) or win_launcher
        rx, ry = lc.get("ok_rel_x", 0.74), lc.get("ok_rel_y", 0.96)
        x = win_launcher.left + int(win_launcher.width * rx)
        y = win_launcher.top + int(win_launcher.height * ry)
        log(f"Clic en OK por COORDS RELATIVAS -> pixel ({x},{y}) "
            f"[rel {rx:.3f},{ry:.3f}].")
        pyautogui.moveTo(x, y, duration=0.3)
        pyautogui.click()
        ok_clicado = True

    # 3) Margen para que arranque la carga del juego.
    # EN: 3) Margin for the game load to start.
    espera = lc.get("espera_tras_ok", 3)
    log(f"OK pulsado. Espero {espera}s a que el juego empiece a cargar de verdad.")
    time.sleep(espera)

    # Verificacion: la ventana estrecha deberia desaparecer (o crecer a juego).
    # EN: Check: the narrow window should disappear (or grow into the game).
    aun_launcher = encontrar_launcher_config(cfg)
    if aun_launcher is not None:
        log("[AVISO] El launcher de config SIGUE visible tras pulsar OK. "
            "El clic pudo no registrarse (foco/privilegios). Reintento una vez.")
        try:
            aun_launcher.activate()
            time.sleep(0.4)
        except Exception:
            pass
        # Reintento por imagen y luego relativo.
        # EN: Retry by image and then by relative coords.
        reintento_ok = False
        if plantilla.exists():
            try:
                loc = pyautogui.locateOnScreen(str(plantilla), confidence=confianza)
            except Exception:
                loc = None
            if loc is not None:
                cx, cy = pyautogui.center(loc)
                pyautogui.moveTo(cx, cy, duration=0.3)
                pyautogui.click()
                reintento_ok = True
        if not reintento_ok:
            rx, ry = lc.get("ok_rel_x", 0.74), lc.get("ok_rel_y", 0.96)
            x = aun_launcher.left + int(aun_launcher.width * rx)
            y = aun_launcher.top + int(aun_launcher.height * ry)
            pyautogui.moveTo(x, y, duration=0.3)
            pyautogui.click()
        time.sleep(espera)
        if encontrar_launcher_config(cfg) is not None:
            log("[AVISO] El launcher SIGUE ahi tras el reintento. El flujo puede "
                "fallar al no llegar al menu. Revisa foco/privilegios.")
            return False

    log("Launcher de config manejado: OK pulsado, el render del juego deberia "
        "arrancar ahora (phase=MainMenu llegara al log en ~1-2 min).")
    return True


# ES: Trae la ventana de Kenshi al frente (restaurándola si está minimizada); devuelve la ventana o None.
# EN: Brings the Kenshi window to the front (restoring it if minimized); returns the window or None.
def enfocar_kenshi():
    """Trae la ventana de Kenshi al frente para que reciba clics/teclas."""
    win = encontrar_ventana_kenshi()
    if win is None:
        log("[AVISO] No encuentro la ventana de Kenshi por titulo. "
            "Usare coordenadas de pantalla completa como fallback.")
        return None
    try:
        if win.isMinimized:
            win.restore()
        win.activate()
        time.sleep(0.5)
        log(f"Ventana Kenshi enfocada: pos=({win.left},{win.top}) "
            f"size=({win.width}x{win.height}).")
    except Exception as e:
        log(f"[AVISO] No pude activar la ventana de Kenshi: {e}")
    return win


# ES: Convierte coordenadas relativas (0..1) de la ventana (o de la pantalla) a píxeles.
# EN: Converts relative (0..1) window (or screen) coordinates to pixels.
def rel_a_pixel(win, rel_x, rel_y):
    """Convierte coords relativas (0..1) a pixeles de pantalla via geometria
    de la ventana de Kenshi (o pantalla completa si no hay ventana)."""
    if win is not None:
        x = win.left + int(win.width * rel_x)
        y = win.top + int(win.height * rel_y)
    else:
        sw, sh = pyautogui.size()
        x = int(sw * rel_x)
        y = int(sh * rel_y)
    return x, y


# ======================================================================
# Acciones del flujo (clics y teclas)
# ======================================================================
# EN: Flow actions (clicks and keys)
# ES: Clic en el centro del botón MULTIPLAYER (rectángulo relativo de config.json).
# EN: Click in the center of the MULTIPLAYER button (relative rectangle from config.json).
def clic_boton_multiplayer(cfg, win):
    """Clica el CENTRO del boton MULTIPLAYER del menu principal."""
    c = cfg["coordenadas_boton_multiplayer"]
    centro_rx = c["rel_x"] + c["rel_w"] / 2.0
    centro_ry = c["rel_y"] + c["rel_h"] / 2.0
    x, y = rel_a_pixel(win, centro_rx, centro_ry)
    log(f"Clic en boton MULTIPLAYER -> pixel ({x},{y}) [rel {centro_rx:.3f},{centro_ry:.3f}].")
    pyautogui.moveTo(x, y, duration=0.4)
    pyautogui.click()
    time.sleep(1.5)


# ES: Clic en CONTINUE (coordenadas de config.json): carga el save forzado.
# EN: Click on CONTINUE (coordinates from config.json): loads the forced save.
def clic_continue(cfg, win):
    """Clica el boton CONTINUE (primer item del menu). Coords desde config."""
    c = cfg["coordenadas_boton_continue"]
    x, y = rel_a_pixel(win, c["rel_x"], c["rel_y"])
    log(f"Clic en CONTINUE -> pixel ({x},{y}). Cargara el save y entrara al mundo.")
    pyautogui.moveTo(x, y, duration=0.4)
    pyautogui.click()


# ES: Plan B de conexión: abre el chat/consola del mod con Enter y escribe "/connect <ip>".
# EN: Connection fallback: opens the mod chat/console with Enter and types "/connect <ip>".
def consola_connect(cfg, win):
    """Red de seguridad: abre la consola (Enter) y manda '/connect <ip>'."""
    ip = cfg["red"]["server_ip"]
    log(f"Red de seguridad: enviando '/connect {ip}' por la consola del mod.")
    enfocar_kenshi()
    time.sleep(0.5)
    pyautogui.press("enter")
    time.sleep(0.6)
    pyautogui.typewrite(f"/connect {ip}", interval=0.04)
    time.sleep(0.3)
    pyautogui.press("enter")
    time.sleep(0.5)


# ES: Intento de ataque "best-effort": clic izquierdo y derecho en un punto relativo de la pantalla.
# EN: Best-effort attack attempt: left and right click on a relative screen point.
def intentar_ataque(cfg, win):
    """Best-effort: clic izq (foco) + clic der (orden de ataque) en el centro."""
    a = cfg["ataque"]
    if not a.get("habilitado", True):
        log("Ataque desactivado en config. Salto este paso.")
        return
    x, y = rel_a_pixel(win, a["clic_rel_x"], a["clic_rel_y"])
    log(f"Intento de ATAQUE: clic-derecho sobre ({x},{y}) buscando un NPC.")
    enfocar_kenshi()
    time.sleep(0.3)
    pyautogui.moveTo(x, y, duration=0.4)
    pyautogui.click(button="left")
    time.sleep(0.5)
    pyautogui.click(button="right")
    time.sleep(0.5)
    log("Orden de ataque enviada (best-effort). El log dira si hubo combate.")


# ======================================================================
# Resumen final del log (veredicto)
# ======================================================================
# EN: Final log summary (verdict)
# ES: Imprime fases alcanzadas, recuento de [DIAG-PAUSE]/[DIAG-FAC], señales de error en la cola del log y un veredicto.
# EN: Prints reached phases, [DIAG-PAUSE]/[DIAG-FAC] counts, error signals in the log tail and a verdict.
def resumen_log(ruta, fases):
    """
    Resume el resultado: que fases se alcanzaron (por log) + conteo de
    DIAG-PAUSE / DIAG-FAC + senales OK/fallo en la cola del log.
    """
    print("\n" + "=" * 70)
    print("  RESULTADO DEL CICLO DE PRUEBA")
    print("=" * 70)

    # Veredicto por fases (lo mas fiable: viene del propio mod).
    # EN: Verdict by phases (most reliable: it comes from the mod itself).
    print("  FASES ALCANZADAS (segun el log del mod):")
    for nombre, alcanzada in fases.items():
        marca = "[OK]" if alcanzada else "[--]"
        print(f"    {marca} {nombre}")

    if ruta is None or not ruta.exists():
        print("-" * 70)
        print("  [!!] No se encontro el log de sesion. Sin detalle adicional.")
        print("=" * 70 + "\n")
        return

    try:
        with open(ruta, "r", encoding="utf-8", errors="ignore") as f:
            lineas = f.readlines()
    except Exception as e:
        print(f"  [!!] No pude leer el log {ruta}: {e}")
        print("=" * 70 + "\n")
        return

    # Conteo global de las marcas DIAG (la tarea pide encontrarlas).
    # EN: Global count of the DIAG markers (the task asks to find them).
    n_pause = sum(1 for ln in lineas if "[DIAG-PAUSE]" in ln)
    n_fac = sum(1 for ln in lineas if "[DIAG-FAC]" in ln)

    # Ultimos ejemplos de cada DIAG.
    # EN: Latest examples of each DIAG.
    ej_pause = [ln.strip() for ln in lineas if "[DIAG-PAUSE]" in ln][-2:]
    ej_fac = [ln.strip() for ln in lineas if "[DIAG-FAC]" in ln][-2:]

    # Senales de fallo en la cola.
    # EN: Failure signals in the tail.
    senales_fallo = [r"\[error\]", r"crash", r"Disconnected",
                     r"Connection failed", r"assert"]
    cola = lineas[-400:]
    fallo_hits = []
    for ln in cola:
        for pat in senales_fallo:
            if re.search(pat, ln, re.IGNORECASE):
                fallo_hits.append(ln.strip())
                break

    print("-" * 70)
    print(f"  Log analizado: {ruta}")
    print(f"  Tamano: {ruta.stat().st_size/1024/1024:.1f} MB | lineas: {len(lineas)}")
    print(f"  [DIAG-PAUSE] encontradas: {n_pause}")
    for h in ej_pause:
        print(f"    > {h[:150]}")
    print(f"  [DIAG-FAC] encontradas: {n_fac}")
    for h in ej_fac:
        print(f"    > {h[:150]}")
    print(f"  Senales de FALLO/ERROR (cola): {len(fallo_hits)}")
    for h in fallo_hits[-6:]:
        print(f"    [!!] {h[:150]}")
    print("-" * 70)

    # Veredicto resumido.
        # EN: Short verdict.
    llego_mundo = fases.get("Entro al mundo (gameLoaded=true)", False)
    if llego_mundo and (n_pause > 0 or n_fac > 0):
        print("  VEREDICTO: OK — el autotest LLEGO AL MUNDO y el log tiene DIAG.")
        print("             Onyx puede leer DIAG-PAUSE/DIAG-FAC para probar fixes.")
    elif llego_mundo:
        print("  VEREDICTO: PARCIAL — llego al mundo pero no se vieron lineas DIAG.")
    else:
        print("  VEREDICTO: FALLO — no se confirmo entrada al mundo por log.")
    print(f"  Log completo para detalle -> {ruta}")
    print("=" * 70 + "\n")


# ======================================================================
# Orquestacion principal
# ======================================================================
# EN: Main orchestration
# ES: Flujo principal: argumentos, configuración y los pasos 1-9 del ciclo de prueba.
# EN: Main flow: arguments, configuration and steps 1-9 of the test cycle.
def main():
    parser = argparse.ArgumentParser(description="Autotest del mod co-op de Kenshi (v2).")
    parser.add_argument("--no-attack", action="store_true",
                        help="Entra al mundo pero NO intenta atacar.")
    parser.add_argument("--dry-run", action="store_true",
                        help="Solo muestra el plan, no lanza nada.")
    parser.add_argument("--keep-open", action="store_true",
                        help="No mata Kenshi al terminar (deja el juego abierto).")
    parser.add_argument("--no-server", action="store_true",
                        help="No lanza el server (asume que ya esta corriendo).")
    args = parser.parse_args()

    cfg = cargar_config()
    t = cfg["tiempos_segundos"]
    d = cfg["deteccion_log"]
    poll = t["poll_intervalo"]

    print("\n" + "#" * 70)
    print("#  AUTOTEST KENSHI CO-OP v2  —  ciclo de prueba autonomo (guiado por log)")
    print("#" * 70)
    log(f"Save que cargara CONTINUE: {cfg['save']['continue_save']}")
    log(f"Server: 127.0.0.1:{cfg['red']['server_port']}")

    if args.dry_run:
        log("[DRY-RUN] Plan:")
        log("  1) matar Kenshi  2) asegurar server  3) forzar continue=<save>")
        log("  4) lanzar Kenshi  4b) MANEJAR LAUNCHER de config (clic OK)")
        log("  4c) recolocar ventana del juego  5) ESPERAR phase=MainMenu (log)")
        log("  6) clic MULTIPLAYER + ESC + clic CONTINUE")
        log("  7) ESPERAR gameLoaded=true (log)  8) ESPERAR connected=true / /connect")
        log("  9) ataque (opcional)  10) resumir log (DIAG-PAUSE/DIAG-FAC)")
        log("[DRY-RUN] No se ha lanzado nada. Fin.")
        return

    # Estado de fases para el veredicto final.
    # EN: Phase state for the final verdict.
    fases = {
        "Menu principal visible (phase=MainMenu)": False,
        "Entro al mundo (gameLoaded=true)": False,
        "Conectado al server (connected=true)": False,
    }

    # 1. Limpieza (solo Kenshi)
    # EN: 1. Cleanup (Kenshi only)
    matar_kenshi()
    time.sleep(2)

    # 2. Server (solo si no corre)
    # EN: 2. Server (only if not running); the continue save is forced first
    forzar_continue_save(cfg)
    asegurar_server(cfg, tocar_server=not args.no_server)

    # 3. Lanzar Kenshi + localizar su log
    # EN: 3. Launch Kenshi + locate its log
    ts_lanzamiento = time.time()
    lanzar_kenshi(cfg)
    ruta_log = esperar_log_nuevo(cfg, ts_lanzamiento, timeout=30)

    # 3b. *** PASO CRITICO 1 *** Manejar el LAUNCHER de config (pista de Zero).
    #     Al lanzar kenshi_x64.exe SIEMPRE sale primero un dialogo de config con
    #     un boton OK. El render del juego NO arranca (y phase=MainMenu NO llega
    #     al log) hasta pulsar ese OK. El intento anterior se quedaba atascado
    #     aqui. Pulsamos OK (por imagen boton_ok.png o coords relativas).
    # EN: 3b. *** CRITICAL STEP 1 *** Handle the config LAUNCHER.
    #     Launching kenshi_x64.exe ALWAYS shows a config dialog first with
    #     an OK button. The game renderer does NOT start (and phase=MainMenu does NOT reach
    #     the log) until that OK is pressed. The previous attempt got stuck
    #     here. We press OK (by image boton_ok.png or relative coords).
    manejar_launcher_config(cfg)

    # 3c. *** PASO CRITICO 2 *** Recolocar la ventana del JUEGO a pantalla
    #     visible (tras el OK, el juego abre su ventana grande). Si arranca
    #     off-screen, su render se queda parado. Moverla a (0,0) 1920x1080
    #     despierta el render. Reintenta unos segundos por si tarda en crearse.
    # EN: 3c. *** CRITICAL STEP 2 *** Move the GAME window to the visible screen
    #     (after OK, the game opens its big window). If it starts
    #     off-screen, its renderer stalls. Moving it to (0,0) 1920x1080
    #     wakes the renderer up. Retries for a few seconds in case it is slow to appear.
    recolocar_ventana_kenshi(timeout=25)

    # 4. Esperar al menu principal POR LOG (no por sleep ciego)
    #    IMPORTANTE: el patron phase=MainMenu SOLO aparece cuando Kenshi YA esta
    #    renderizando el menu de verdad (no en splash/carga). En la sesion buena
    #    tardo ~37s desde el arranque del mod. Si NO aparece, NO clicamos a ciegas:
    #    un clic en la ventana de splash (pequena) puede pulsar Quit y cerrar el
    #    juego. Abortamos limpiamente para no romper nada.
    # EN: 4. Wait for the main menu BY LOG (not by blind sleep)
    #     IMPORTANT: the phase=MainMenu pattern ONLY appears when Kenshi IS ALREADY
    #     rendering the real menu (not splash/loading). In the good session
    #     it took ~37s from mod start. If it does NOT appear, we do NOT click blindly:
    #     a click on the (small) splash window could press Quit and close the
    #     game. We abort cleanly so nothing breaks.
    hit = esperar_patron_en_log(ruta_log, d["patron_menu_principal"],
                                t["timeout_menu_principal"], poll,
                                "menu principal (phase=MainMenu)")
    if hit:
        fases["Menu principal visible (phase=MainMenu)"] = True
        time.sleep(2.0)  # margen para que el menu termine de pintar
    else:
        log("[ABORTO] No se detecto phase=MainMenu en el log dentro del timeout. "
            "Kenshi sigue en carga/splash o se colgo. NO clico a ciegas (un clic "
            "en el splash podria cerrar el juego). Reviso el log y termino.")
        resumen_log(ruta_log, fases)
        if not args.keep_open:
            log("Cerrando Kenshi (el server se deja corriendo)...")
            matar_kenshi()
        log("Ciclo abortado en fase de menu. Sube timeout_menu_principal si tu "
            "PC carga Kenshi mas lento, o revisa por que no llega al menu.")
        return

    # 5. Clic MULTIPLAYER -> ESC -> CONTINUE
    #    Re-medimos la ventana JUSTO antes de clicar y verificamos que tiene un
    #    tamano de menu real (>800px ancho). Si es pequena, sigue en splash.
    # EN: 5. Click MULTIPLAYER -> ESC -> CONTINUE
    #     We re-measure the window RIGHT before clicking and check it has a
    #     real menu size (>800px wide). If it is small, it is still on the splash.
    win = enfocar_kenshi()
    if win is not None and win.width < 800:
        log(f"[AVISO] La ventana mide {win.width}x{win.height} (demasiado "
            f"pequena para el menu real). Espero 8s extra a que termine de cargar.")
        time.sleep(8)
        win = enfocar_kenshi()
    clic_boton_multiplayer(cfg, win)
    log("ESC para cerrar el panel del mod y volver al menu principal.")
    pyautogui.press("esc")
    time.sleep(1.0)
    win = enfocar_kenshi()  # re-enfocar por si el panel robo foco
    clic_continue(cfg, win)

    # 6. Esperar a ENTRAR AL MUNDO por log (gameLoaded=true)
    # EN: 6. Wait to ENTER THE WORLD by log (gameLoaded=true)
    hit = esperar_patron_en_log(ruta_log, d["patron_mundo_cargado"],
                                t["timeout_entrar_mundo"], poll,
                                "entrada al mundo (gameLoaded=true)")
    if hit:
        fases["Entro al mundo (gameLoaded=true)"] = True
    else:
        log("[AVISO] No se confirmo gameLoaded=true por log dentro del timeout.")

    # 7. Esperar conexion al server (connected=true). Si no, /connect manual.
    # EN: 7. Wait for the server connection (connected=true). Otherwise, manual /connect.
    hit = esperar_patron_en_log(ruta_log, d["patron_conectado"],
                                t["timeout_conexion"], poll,
                                "conexion al server (connected=true)")
    if hit:
        fases["Conectado al server (connected=true)"] = True
    else:
        log("Auto-connect no confirmado; mando /connect como red de seguridad.")
        consola_connect(cfg, win)
        hit = esperar_patron_en_log(ruta_log, d["patron_conectado"],
                                    20, poll, "conexion tras /connect manual")
        if hit:
            fases["Conectado al server (connected=true)"] = True

    # 8. Ataque (opcional, best-effort)
    # EN: 8. Attack (optional, best-effort)
    if not args.no_attack and fases["Entro al mundo (gameLoaded=true)"]:
        time.sleep(t["espera_antes_ataque"])
        win = enfocar_kenshi()
        intentar_ataque(cfg, win)
        time.sleep(3)

    # 9. Resultado
    # EN: 9. Result
    log("Generando resumen del log (fases + DIAG-PAUSE/DIAG-FAC)...")
    resumen_log(ruta_log, fases)

    # Cierre: matar SOLO Kenshi (el server se deja como pide la tarea).
    # EN: Shutdown: kill ONLY Kenshi (the server is left running as the task asks).
    if args.keep_open:
        log("--keep-open: dejo Kenshi abierto. Cierralo tu cuando acabes.")
    else:
        log("Cerrando Kenshi (el server se deja corriendo)...")
        matar_kenshi()

    log("Ciclo de prueba terminado.")


if __name__ == "__main__":
    # ES: Punto de entrada: avisa si el script no corre como administrador (los clics podrían no llegar
    #     a un Kenshi elevado) y lanza main().
    # EN: Entry point: warns if the script is not running as administrator (clicks might not reach an
    #     elevated Kenshi) and runs main().
    try:
        es_admin = ctypes.windll.shell32.IsUserAnAdmin()
        if not es_admin:
            log("[NOTA] Si Kenshi se ejecuta como administrador y este script no, "
                "los clics podrian no registrarse.")
    except Exception:
        pass
    main()
