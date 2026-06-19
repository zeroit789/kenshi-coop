# Autotest — Kenshi Co-op (prueba autónoma del mod)

Herramientas EXTERNAS para que **Onyx (Claude) pruebe el mod co-op de Kenshi sin que un humano juegue a mano**. No tocan el código del mod (KenshiMP.Core / Scanner), solo lo lanzan y leen sus logs.

> **TL;DR para Onyx:**
> 1. Validar la capa de red primero (fiable): `run_autotest.bat` → no, eso es GUI. Para red usa `headless_test.py --integration` (requiere exes de test compilados).
> 2. Probar el flujo real dentro del juego (frágil): `run_autotest.bat`
> 3. Si los clics caen mal: `calibrar.py` para sacar las coordenadas reales.
> 4. El veredicto de éxito/fallo lo da el **log** `KenshiOnline_<PID>.log`, que el script localiza y resume solo.

---

## Qué automatiza

El ciclo de prueba pedido:

1. Lanza el server (`KenshiMP.Server.exe`) y Kenshi (`kenshi_x64.exe`).
2. Navega el menú: clic en **MULTIPLAYER** → carga partida con **CONTINUE** → entra al mundo.
3. Una vez en el mundo: intenta **atacar a un NPC** (clic).
4. Localiza y **resume el log** `KenshiOnline_<PID>.log` para que Onyx lea el resultado.

---

## Decisión técnica: por qué esta vía (evaluación de las 3 opciones)

| Vía | Veredicto | Motivo |
|-----|-----------|--------|
| **1. GUI automation (pyautogui)** | ✅ **ELEGIDA** para el flujo dentro del juego | Es la única forma de probar el flujo real de usuario. Frágil, pero mitigada (ver abajo). |
| **2. CLI args de `kenshi_x64.exe`** | ❌ **IMPOSIBLE** | Verificado a nivel de binario: el exe **ni siquiera importa `GetCommandLineW`/`CommandLineToArgvW`**. No parsea ningún argumento. No hay `-load`, `-window`, `-nointro`, nada. Kenshi nunca soportó launch options. |
| **3. Save reutilizable** | ✅ **ELEGIDA como atajo clave** | No hay carga directa por CLI, pero el sistema **"Continue"** de Kenshi sí carga un save de un clic. El script fuerza `continue=<save>` en `settings.cfg` y luego clica CONTINUE → **se salta TODA la creación de personaje** (lo más frágil de automatizar). |

**Combinación final:** GUI automation (vía 1) + truco del save Continue (vía 3). El cuello de botella real no era "atacar al NPC", sino crear/cargar partida; el save Continue lo elimina.

### Datos que hacen esto fiable (sacados del código del mod, no adivinados)

- **El mod se carga solo**: está registrado como plugin de Ogre en `Plugins_x64.cfg` (`Plugin=KenshiMP.Core`). Lanzar `kenshi_x64.exe` directo ya carga el mod. No hace falta inyector ni Steam.
- **Coordenadas del botón MULTIPLAYER conocidas**: definidas en `KenshiMP.Core/hooks/render_hooks.cpp` como coords relativas (`MP_BTN_X=0.260417`, `MP_BTN_Y=0.582407`, `W=0.15625`, `H=0.0638889`). El script clica el centro. Al ser relativas, funcionan a cualquier resolución.
- **Conexión automática**: el mod tiene **auto-connect** a `127.0.0.1:27800` tras cargar el save (2s de espera). Como red de seguridad, el script además manda `/connect 127.0.0.1` por la consola.
- **Consola del mod**: se abre con **Enter** (chat). Comandos útiles: `/connect`, `/sync`, `/status`, `/players`, `/pos`, `/tp`, `/chars`. (Definidos en `KenshiMP.Core/sys/builtin_commands.cpp`.)
- **Hotkeys del mod**: **F1** = menú MP, **Tab** = lista jugadores, **Insert** = panel de log, **`** (backtick) = debug HUD.

---

## Estado del entorno (verificado)

- Kenshi 1.0.68, resolución **1920x1080 en modo VENTANA** (`kenshi.cfg` → `Full Screen=No`). ✅ requisito cumplido.
- Saves jugables existentes en `C:\Users\Zero\AppData\Local\kenshi\save\`: `autosave0`, `autosave1`, `autosave2`, `dada`. El config usa `autosave0` por defecto.
- `server.json` (en el game dir) ya configurado: puerto 27800, master 162.248.94.149:27801, 16 jugadores.
- Dependencias Python instaladas: `pyautogui`, `pygetwindow`, `numpy`, `opencv`, `pillow`, `mss`.

---

## Scripts

| Archivo | Qué es |
|---------|--------|
| `autotest_kenshi.py` | **Orquestador principal.** Ciclo completo: server → Kenshi → menú → mundo → ataque → log. |
| `run_autotest.bat` | Lanzador del orquestador con el Python 3.12 del sistema. **Onyx ejecuta esto.** |
| `config.json` | Toda la configuración: rutas, save, tiempos, coordenadas. Editable sin tocar código. |
| `calibrar.py` | Calibrador de coordenadas. Úsalo si los clics caen en sitio equivocado. |
| `headless_test.py` | Pruebas de **red sin GUI** usando los exes de test del proyecto (más fiable para la capa de protocolo). |

---

## Cómo lo lanza Onyx

### Opción A — Flujo completo dentro del juego (GUI)

```bat
cd /d E:\Aplicaciones\Kenshi-Online\tools\autotest
run_autotest.bat
```

Variantes:
```bat
run_autotest.bat --dry-run      :: solo muestra el plan, no lanza nada (para comprobar config)
run_autotest.bat --no-attack    :: entra al mundo pero no intenta atacar
run_autotest.bat --keep-open    :: deja Kenshi y el server abiertos al terminar
```

Al final imprime algo como:
```
======================================================================
  RESULTADO DEL CICLO DE PRUEBA
======================================================================
  Log analizado: E:\...\Kenshi\KenshiOnline_12345.log
  Senales de EXITO encontradas: N   [OK] ...Connected... / ...handshake...
  Senales de FALLO/ERROR encontradas: M   [!!] ...[error]... / ...crash...
======================================================================
```
Onyx lee ese resumen y, si necesita detalle, abre el log completo en la ruta indicada.

### Opción B — Validar la red sin GUI (recomendado primero)

```bat
cd /d E:\Aplicaciones\Kenshi-Online\tools\autotest
"C:\Users\Zero\AppData\Local\Programs\Python\Python312\python.exe" headless_test.py --integration
```

Corre los **15 tests de protocolo** (conexión, handshake, spawn, sync posición, chat, combate, etc.) y reporta PASS/FAIL. Mucho más fiable que la GUI porque no depende de clics.

> ⚠️ **Requiere los exes de test compilados.** Ahora mismo **NO están compilados**
> (`KenshiMP.IntegrationTest.exe` y `KenshiMP.TestClient.exe` no existen en el game dir).
> Para generarlos hay que compilar el proyecto (`cmake --build build --config Release`)
> con esos targets. El server (`KenshiMP.Server.exe`) sí está compilado.

### Opción C — Bot patrullero junto a un Kenshi real

```bat
"C:\...\python.exe" headless_test.py --client --name OnyxBot
```
Lanza server + un bot que conecta y patrulla. Útil para ver, junto a un Kenshi real conectado, si el bot remoto aparece y se mueve. (También requiere `KenshiMP.TestClient.exe` compilado.)

---

## Si la GUI automation falla (calibración)

La automatización de GUI **es frágil**. Si los clics no caen donde deben:

1. Arranca Kenshi a mano hasta el menú principal.
2. Ejecuta:
   ```bat
   "C:\...\python.exe" calibrar.py --shot
   ```
3. Pasa el ratón por encima del botón (ej. CONTINUE). El script imprime su posición en pixel y en coords relativas.
4. Copia los valores relativos al `config.json`:
   - Botón MULTIPLAYER → sección `coordenadas_boton_multiplayer` (ya correcto del código del mod).
   - Botón CONTINUE → está hardcodeado en `autotest_kenshi.py` (`clic_continue`, rel `0.338542, 0.40`); ajústalo ahí si tu menú difiere.
5. `--shot` guarda `debug_screenshot.png` para inspección visual de dónde está el cursor.

Ajusta también los **tiempos** en `config.json` → `tiempos_segundos` si tu PC carga Kenshi más lento (sube `espera_arranque_juego`, `espera_intro_splash`, `espera_tras_continue`).

---

## HALLAZGOS VERIFICADOS 2026-06-18 (validación end-to-end real)

Tras ejecutar el ciclo de verdad muchas veces y diagnosticar paso a paso, esto es lo que se sabe con CERTEZA (probado, no supuesto):

### Lo que SÍ funciona (fiable)
1. **Arranque + carga del mod**: lanzar `kenshi_x64.exe` carga el mod (plugin Ogre). ✅
2. **Launcher de config (descubrimiento clave)**: al lanzar Kenshi aparece PRIMERO un diálogo de config (Direct3D/resolución/idioma/**OK**), ~488x572. **El render del juego NO arranca (ni `phase=MainMenu` llega al log) hasta pulsar ese OK.** Lo maneja `manejar_launcher_config()` (clic OK por imagen `boton_ok.png` o coords relativas). ✅
3. **Recolocación de ventana**: Kenshi puede abrir su ventana fuera de pantalla. `recolocar_ventana_kenshi()` la fuerza a (0,0) 1920x1080. ✅
4. **Llegada al menú principal**: tras el OK, el juego renderiza y llega a `phase=MainMenu` de forma fiable (se ve en el log: cientos de `render_hooks: frame=... phase=MainMenu`). ✅
5. **Lectura del log en vivo**: el autotest detecta cada fase por log (`phase=MainMenu/Loading/Connecting/Connected`, `gameLoaded=true`, `connected=true`, `[DIAG-PAUSE]`, `[DIAG-FAC]`) en vez de esperas ciegas. Esto es robusto. ✅
6. **Input de TECLADO**: el mod instala un **WndProc hook** (`render_hooks: ... passthrough + WndProc`) y **SÍ capta teclado sintético**. Verificado: `F1` abre el `NativeMenu` MP del mod (`NativeMenu: Shown`), `Insert` togglea el panel de log (`NativeHud: Log panel OFF`). La **consola** del mod (Enter → `/connect`, `/status`...) es accesible por teclado. ✅

### EL MURO (lo que NO funciona, con evidencia)
**El menú NATIVO de Kenshi (MyGUI + OIS/DirectInput) IGNORA todo input de ratón sintético.** Probado exhaustivamente sobre un Kenshi vivo, enfocado, ventana a 0,0 1920x1080, con coordenadas verificadas VISUALMENTE sobre los botones reales:
- `mouse_event` (Win32) → el menú no reacciona, sigue en `phase=MainMenu`.
- `pyautogui.click()` → idem.
- `SendInput` con coords absolutas → idem.
- Clic en CONTINUAR y en MULTIPLAYER (coords reales medidas por análisis de píxeles) → **0 reacción** en todos los casos.

Y el menú nativo **tampoco responde a teclado** (flechas/Enter/Espacio no mueven la selección ni activan CONTINUAR).

**Consecuencia:** no se puede pulsar CONTINUAR por automación → **no se puede entrar al mundo por la vía GUI.** El cuello de botella NO son los timings ni las coordenadas (ambos resueltos): es que OIS, el sistema de input de Kenshi, descarta el input que no viene del driver de hardware real. Esto es una limitación del propio juego, no del script.

> **Matiz honesto (no probado al 100%):** las pruebas se hicieron con sesión NO admin, Kenshi lanzado DIRECTO (no por Steam), mismo nivel de privilegios → UIPI descartado como causa. NO se probó: (a) lanzar todo elevado como administrador, (b) Kenshi lanzado vía Steam (App 233860) en vez de directo, (c) input por driver virtual (p.ej. Interception, vJoy). Si alguna de esas cambia el comportamiento, la vía GUI podría reabrirse. Pero el patrón (teclado SÍ vía WndProc del mod, ratón NO vía OIS) apunta fuerte a OIS descartando input sintético, que es independiente de privilegios.

> **Nota sobre coordenadas:** las coords de MULTIPLAYER del config vienen de `render_hooks.cpp` (rel_y 0.582) pero el botón VISUAL real está en rel_y ≈ 0.404 (medido). Da igual: corregirlas no ayuda porque el menú ignora el clic de todas formas.

### Veredicto: ¿puede Onyx probar fixes de noche con el autotest GUI?
**NO de forma 100% autónoma con el menú nativo.** El autotest llega de forma fiable hasta el MENÚ y lee el log perfectamente, pero **no puede cruzar el menú para entrar al mundo** sin un clic de hardware real. Para autonomía total de noche, la vía correcta es la **HEADLESS** (abajo), que no toca el menú.

### Recomendación REAL (vía headless, sin tocar el menú)
La forma fiable de que Onyx pruebe fixes sin un humano es **compilar los exes de test** y usar `headless_test.py --integration`. Estos exes NO existen ahora (`KenshiMP.IntegrationTest.exe`, `KenshiMP.TestClient.exe`). Generarlos requiere:
```
cmake --build build --config Release --target KenshiMP.IntegrationTest KenshiMP.TestClient
```
⚠️ **OJO (coordinación):** otro agente edita `KenshiMP.Core` y sus targets CMake. **NO compilar esos targets sin coordinarse** para no pisar su build. Por eso aquí solo se DOCUMENTA la vía; no se compila.

Mientras tanto, lo que SÍ aporta valor autónomo HOY:
- El autotest deja Kenshi **en el menú con el mod cargado y el log activo**. Onyx puede leer el log (arranque del mod, hooks instalados, errores de inicialización) para validar que un fix de Core no rompe la carga.
- Por **teclado** (que sí funciona) Onyx puede abrir el `NativeMenu` MP del mod (F1) y la consola — útil si esos paneles del mod (no el menú nativo) aceptan navegación por teclado para Host/Join (pendiente de confirmar widget a widget).

---

## Limitaciones (honestidad, sin adornos)

- **GUI automation = frágil.** Depende de timings (Kenshi tarda en cargar) y de que la ventana tenga el foco. Si una espera se queda corta, los clics caen en pantallas equivocadas y el resto del flujo se descarrila. Por eso los tiempos son generosos y configurables.
- **El "ataque" es best-effort.** El script hace clic-izquierdo (foco) + clic-derecho (orden de ataque) en el centro de pantalla. NO garantiza que haya un NPC ahí ni que la mecánica de selección sea exacta. **El veredicto real de si el combate funcionó lo da el log del mod** (hooks `CombatHit`/`CombatDeath`), no el script. Según `docs/TESTING.md`, el combate tiene problemas conocidos (las barras de daño no sincronizan, el hook `ApplyDamage` puede crashear).
- **La vía headless es más fiable que la GUI**, pero **necesita compilar los exes de test** que ahora no existen.
- **Privilegios:** si Kenshi corre como administrador y el script no, los clics sintéticos pueden no llegar a la ventana. El script avisa de esto al arrancar.
- **El foco de ventana** se intenta con `pygetwindow.activate()`, pero Windows a veces lo bloquea. Si falla, el script cae a coordenadas de pantalla completa (asume Kenshi maximizado/sin bordes).

---

## Recomendación de uso para Onyx

1. **Primero** (cuando estén compilados los exes de test): `headless_test.py --integration` para confirmar que la capa de red/protocolo está sana. Rápido y determinista.
2. **Después**: `run_autotest.bat` para el flujo real dentro del juego, y leer el log resultante.
3. Si la GUI falla → `calibrar.py` y ajustar `config.json`.
