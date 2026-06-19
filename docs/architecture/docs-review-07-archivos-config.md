# docs-review-07 — Archivos sueltos, notas, config y herramientas de la raíz

> Revisión READ-ONLY de los ficheros sueltos de la raíz de `E:\Aplicaciones\Kenshi-Online`,
> la carpeta `KenshiMP.MasterServer`, `tools/` y `dist/`. Fecha: 2026-06-18.
> No se modifica código. Objetivo: extraer notas del autor, config, TODOs y pistas para el
> combate congelado del host.

---

## 0. TL;DR

1. **`for the project.txt` y `open the build folder.txt` están VACÍOS (0 bytes).** No hay
   notas/ideas del autor original ahí. Las "notas del autor" reales viven en `CONTRIBUTING.md`,
   `CREDITS.md`, los audits de `docs/architecture/` y los scripts de `tools/re_task/` (todo de hoy).
2. **`settings.cfg`** = solo idioma del juego (`language=en_GB`, `steam_language=english`). No es
   config del mod; es el `settings.cfg` de Kenshi. Irrelevante para el combate.
3. **La pista MÁS fuerte para el combate congelado NO está en los ficheros de config**, está en
   `tools/re_task/` (RE del sistema de tareas/órdenes, trabajo de hoy 18:50-18:56) y en los audits
   `audit-08`/`audit-09`/`audit-03`/`COLA-FIXES-CORE.md`. Resumen en §5.
4. **`COLA-FIXES-CORE.md` confirma el estado actual**: la pausa fantasma YA está resuelta
   (`subsysCache=0`, `officialSetter=yes`), pero el combate sigue roto porque **la orden de atacar
   no llega a `StartAttack 0x7A1650` (el hook no se dispara)**, las tareas de char están congeladas
   (levantarse/recuperarse de KO), y los NPCs huyen.

---

## 1. Ficheros sueltos de la raíz

| Fichero | Contenido / utilidad |
|---|---|
| `for the project.txt` | **VACÍO (0 bytes).** Sin notas del autor. |
| `open the build folder.txt` | **VACÍO (0 bytes).** Nombre = recordatorio de abrir `build/`. |
| `settings.cfg` | `settings.cfg` de Kenshi (idioma). 2 líneas: `language=en_GB` / `steam_language=english`. No es del mod. |
| `faction-slots.json` | Mapea las 6 facciones del jugador a sus `.mod` de slot (`10/12/45/46/47/48-kenshi-online.mod`). Ligado al selector de facciones (COLA #4). |
| `README.md` | Bilingüe. Estado: builds OK, 95/95 unit tests, **co-op in-game (detección/spawn/sync) "se está validando y arreglando"**. Plugin de Ogre3D (sin inyección externa), direcciones por string-xref, ENet UDP, server-authoritative. Controles in-game (F1 menú, Tab lista, etc.) y comandos de chat (`/connect`, `/tp`, `/players`…). |
| `CMakeLists.txt` | Proyecto `KenshiMP v0.1.0`, C++17, x64 forzado. **Clave: `-D_ITERATOR_DEBUG_LEVEL=0`** para compatibilidad ABI con los DLL Release de Kenshi (MyGUI/Ogre) — sin esto, los `std::string/vector` no casan a través del borde del DLL. 10 subproyectos. |
| `CMakePresets.json` | Presets `x64-release` (default), `x64-debug` (mantiene IDL=0 por ABI), `x64-release-vs2019`. |
| `build.bat` | Build completo: detecta VS 2022/2019, inicializa submódulos `lib/`, configura, compila Release y corre `KenshiMP.UnitTest.exe`. |
| `setup.bat` | Ruta alternativa vía **vcpkg** (enet/minhook/imgui/json/spdlog). Convive con los submódulos `lib/`. |
| `install.bat` (raíz) | Despliega DLL+Server a la carpeta de Kenshi, parchea `Plugins_x64.cfg`, comprueba layouts. Asume Kenshi como carpeta padre (`%~dp0..`). |
| `test_multiplayer.bat` | "Thread Safety Fix - Phase 1". Mata procesos, arranca server **en puerto 7777** (¡ojo, distinto del 27800 del dist/!), lanza 2 instancias de Kenshi desde `C:\Program Files (x86)\Steam\...\Kenshi` y monitoriza crashes. **Ruta de Steam hardcodeada a `C:\` — NO es la del PC de Zero (`E:\SteamLibrary\...\Kenshi`).** |
| `deploy-onyx.ps1` | Script de Onyx (2026-06-17). Cierra juego/server/master, copia `Core.dll`+`Server.exe`+`MasterServer.exe`+layouts a `E:\SteamLibrary\steamapps\common\Kenshi`, verifica bytes/fecha del DLL. **Esta es la ruta real del juego de Zero.** |

### Inconsistencias detectadas en config (a tener en cuenta)
- **Puerto del server divergente:** `dist/server.json` y `JOINING.md` usan **27800**; `test_multiplayer.bat` arranca en **7777**. Master server en **27801**.
- **Ruta de Kenshi divergente:** `test_multiplayer.bat` apunta a `C:\Program Files (x86)\Steam\...` (default genérico), mientras `deploy-onyx.ps1` y los scripts de `tools/` usan la real `E:\SteamLibrary\steamapps\common\Kenshi`. Para pruebas en el PC de Zero, el bat de test está desactualizado.

---

## 2. `dist/` — qué se distribuye al usuario final

Binarios listos: `KenshiMP.Core.dll` (1.3 MB), `KenshiMP.Server.exe`, `KenshiMP.MasterServer.exe`,
`KenshiMP.Injector.exe`, `KenshiMP.IntegrationTest.exe`, `KenshiMP.TestClient.exe`.
Assets: 3 layouts MyGUI (`Kenshi_MainMenu` / `MultiplayerHUD` / `MultiplayerPanel`),
`kenshi-online.mod` (plantillas de personaje para spawn remoto), `install.bat`/`uninstall.bat`, `JOINING.md`.

- **`server.json`** (config por defecto distribuida): puerto 27800, master `162.248.94.149:27801`,
  16 jugadores, tickRate 20, PvP on, autosave a `world.kmpsave`, `gameSpeed 1.0`.
  > El master `162.248.94.149` es el que aparece en COLA-FIXES #3 como conexión molesta del cliente a silenciar en local.
- **`JOINING.md`** ("made with love by fourzerofour"): guía de usuario. Sección Troubleshooting con
  pista útil: jugador remoto invisible → buscar en el debug log (Insert) los mensajes
  `"MOD TEMPLATE"` / `"NPC HIJACK"`; el spawn remoto usa las plantillas de `kenshi-online.mod`.

---

## 3. `KenshiMP.MasterServer/` — registro central para el server browser

`main.cpp` (387 líneas) + `CMakeLists.txt`. Servidor ENet en puerto **27801**. Mantiene un
`unordered_map<"ip:port", RegisteredServer>`; los game servers se registran/heartbeat, los clientes
piden la lista (`MS_QueryList` → `MS_ServerList`), poda servers sin heartbeat >90s. Thread de consola
(`status`/`stop`). Config JSON (`master.json`: port + logFile). **Componente de infraestructura, sin
relación con el combate.** Limpio y autónomo.

---

## 4. `tools/` — herramientas (qué hay)

| Herramienta | Qué hace |
|---|---|
| `re_scanner.py` (51 KB) | Scanner RE general (el grande, base del trabajo de offsets/patrones). |
| `rebase_mod.py`, `fix_mod_gamestarts.py` | Manipulación del `.mod` (rebase de IDs, game starts). Ligado al lío de slots de facción. |
| `ModGen/ModGen/` | Generador de mods (vacío/anidado; sin contenido relevante a la vista). |
| **`re_task/`** (HOY 18:50-18:56) | **RE ACTIVO del sistema de órdenes/tareas del Character.** Ver §5. |
| `disasm_livebranch.py`, `disasm_context.py` (HOY 18:49-18:50) | Desensamblan la "rama viva" del AI tick (`0x5CD1C0`) y el gate del AI tick `0x5CCD90`. Soporte de audit-08. |
| `peek_funcs.py`, `resolve_vtable.py` (HOY) | Inspeccionan prólogos de funciones clave del AI tick y resuelven la vtable de Character por RTTI (`.?AVCharacter@@`). |
| `autotest/` (HOY 03:xx) | **Auto-test gráfico de Kenshi**: `autotest_kenshi.py` (34 KB) + Playwright/pyautogui, screenshots del menú, calibración, `config.json`. Es el intento de automatizar la interacción gráfica (lanzar juego+server, clic en MULTIPLAYER→Join) que la memoria de Zero marca como "falta automatizar". README propio dentro. |

> **Nota técnica recurrente en los scripts:** usan **iced-x86 + parser PE propio** porque
> **capstone está bloqueado por WDAC** en el PC. Si se reusan, mantener iced-x86.

---

## 5. PISTAS PARA EL COMBATE CONGELADO (lo relevante para la consigna)

El trabajo de hoy ha avanzado mucho y ha **descartado pistas falsas**. Estado consolidado:

### 5.1 Lo que YA se descartó (no perseguir)
- **`char+0xD8` NO es la causa** (audit-08). No es un acumulador `+= dt`; es una **caché por-char
  de un valor derivado de la HORA global del juego** (`0x66CB50(reloj)`), recalculada cada AI tick.
  El gate `0.75` estaba documentado **invertido**: el think corre si `+0xD8 < 0.75`, y además hay
  **bypass** si `[vtbl+0x58](char)→[+0x250] != 0`. Forzar +0xD8 no desbloquea nada.
- **H1 (host fuera de la simulación) REFUTADA**: runtime `[DIAG-SIMLIST] hostInSimList=YES`. El host
  SÍ está en el `unordered_set` que itera `updateCharacters 0x786E30`, SÍ recibe el AI tick
  (`0x5CCD90`), pasa el gate `+0x5BC==0` (vivo) y entra a la rama viva `0x5CD1C0`.
- **La pausa fantasma ya está resuelta** (COLA-FIXES #1): `paused(+0x8B9)=0`, `gameSpeed(+0x700)=1.0`,
  `subsysCache=0`, `officialSetter=yes`. La simulación corre.

### 5.2 Hipótesis VIVA #1 — el mod agarra el CHAR EQUIVOCADO (audit-09, la más prometedora)
**SOSPECHA FUNDADA, confirmada por bytes:** Kenshi guarda el char *controlado activo* del jugador en
**`PlayerInterface + 0x2A8` (`controlledChar`)**, escrito por `SetControlledChar` (`0x802520`) como
**el ÚLTIMO miembro de la facción** (`Faction+0x218[Faction+0x210 - 1]`) y leído por el motor en
`0x50E9CF` para aplicar las órdenes del jugador.

**Pero el mod lee otro sitio:** `PI+0x2B0 → data[0]` del lektor `playerCharacters`
(`GetPlayerPrimaryCharacterDirect`, `game_character.cpp:913`) y el primer objeto de su EntityRegistry
(`PlayerController::GetPrimaryCharacter`, `player_controller.cpp:46`). **Ninguno lee `PI+0x2A8`.**

Si divergen (escuadra de >1 char, o NPC contratado entró antes en el lektor), el mod parchea
(faction `+0x10`, seed `+0xD0`) un char que el jugador NO controla → el char real nunca se toca →
**las órdenes se aplican al char real pero los fixes/diagnósticos del mod miran a otro.** Pista que
encaja: `/verify` muestra `char+0x10 (faction) FAIL`.

- **DIAG ya añadido** (`[DIAG-PRIMARY]`, read-only en `core.cpp`): vuelca `data[0]` vs `ctrlChar(PI+0x2A8)`
  vs `facLast`. Si `data[0] != ctrlChar` → CONFIRMADO char equivocado.
- **Fix propuesto (NO aplicado):** resolver el primario por `PI+0x2A8` con fallback a
  `Faction+0x218[count-1]` y luego a `data[0]`. Tocar `game_types.h` (`controlledChar=0x2A8`,
  `FactionOffsets memberCount=0x210/memberArray=0x218`), `game_character.cpp:913`, doc `06-game-offsets.md`.
  > Offsets `+0x2A8/+0x210/+0x218` son de CAMPO (portables Steam/GOG). Las RVA son solo evidencia, no se hookean.

### 5.3 Hipótesis VIVA #2 — la ORDEN no llega a la cola de tareas (capa de comando)
COLA-FIXES #1 + audit-03 lo dicen claro: **la orden de atacar no llega a `StartAttack`, el hook no
se dispara.** Y audit-03 destapa por qué el diagnóstico engaña:
- **El hook "StartAttack" está mal apuntado.** Apunta a `0x7B2A20` = *"Cut/blunt damage calculation"*
  (string ancla `"Cutting damage"`), NO a la función de iniciar ataque. Por eso el DIAG nunca confirma
  si la orden llega. El `0x7A1650` del comentario es residuo de otra hipótesis sin enlazar.
- **Recomendación audit-03 (Opción A, bajo riesgo):** renombrar el hook actual a `CutBluntDamageCalc`
  y re-apuntar el DIAG de combate a `MartialArtsCombat (0x892120)` (handler de combate cuerpo a cuerpo
  ya resuelto), que sí confirma entrada en combate. Sitios: `patterns.h:90/342`, `orchestrator.cpp:169-171`.

El trabajo de hoy en **`tools/re_task/`** persigue exactamente esta capa: RE del **sistema de
tareas/órdenes** (`AI::create 0x622110` escribe `char+0x20 = AITaskSytem*`, confirmado en `0x6221AF`),
la **cola `lektor<Tasker*>` en `AITaskSytem+0x2E8`**, y las vtables `Tasker (0x16BDC68)` /
`GOAPTaskMgr (0x16BC1D8)`. El objetivo es localizar el `addTask`/push real para entender por dónde
debería entrar la orden del jugador y por qué no llega.

### 5.4 Orden de prioridad recomendado (de los audits)
1. **P0** Confirmar con `[DIAG-PRIMARY]` si `data[0] != ctrlChar` (char equivocado, audit-09). Si sí → fix `PI+0x2A8`. **Es el candidato nº1.**
2. **P1** Arreglar el apuntado del hook de combate (audit-03): re-apuntar DIAG a `MartialArtsCombat 0x892120`.
3. **P2** Cerrar la RE de `re_task/` (cola de Tasker/addTask) para entender la ruta de orden.
4. **P3** Los "huyen" → matching de facción por nombre (COLA #6/#7; ids basura ya neutralizados a -1).

---

## 6. Otros TODOs/notas dispersas detectadas

- **COLA-FIXES-CORE.md** (cola secuencial de fixes de Core, "de uno en uno para no pisarse"):
  #1 combate (en curso), #2 spam HUD "Looking for characters... (tracked: 0)", #3 silenciar conexión
  del cliente al master `162.248.94.149`, #4 selector de facciones cliente, #5 bugs de threading
  (UAF en load/reconnect, audit-01), #6 offsets (parcial; health chain pendiente de CE en vivo),
  #7 matching de facciones por nombre.
- **CONTRIBUTING.md** (notas técnicas valiosas del autor): lista los **14 módulos de hook**, convención
  de **MovRaxRsp prologue** (MinHook corrompe RBX → usar naked detour), y "Areas Needing Help" con RVAs:
  `ApplyDamage 0x7A33A0` (needs naked detour), `ItemPickup 0x74C8B0`, `ItemDrop 0x745DE0`,
  `AICreate 0x622110`, `CharacterDeath 0x7A6200`. Versión declarada **0.3.0-alpha** (README dice v0.1.0 en CMake — desincronizado).
- **CREDITS.md**: base directa = `The404Studios/Kenshi-Online` (MIT). RE de referencia =
  `BFrizzleFoShizzle/RE_Kenshi` + `KenshiLib` (GPLv3) para offsets de struct y detección de versión por MD5.
  Deps fijadas por commit en `lib/` (ENet, ImGui, json, MinHook, spdlog).
