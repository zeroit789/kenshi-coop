# Kenshi Co-op

**[ES — Español](#es) | [EN — English](#en)**

[![Status](https://img.shields.io/badge/status-frozen%20%7C%20unstable-red)]()
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-lightgrey)]()
[![Kenshi](https://img.shields.io/badge/Kenshi-1.0.68%20Steam-blue)]()
[![Tests](https://img.shields.io/badge/unit%20tests-95%2F95-brightgreen)]()
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

---

<a id="es"></a>

## Español

> **Proyecto congelado desde el 19-07-2026. No es un mod jugable.**
> Si lo que quieres es jugar a Kenshi en cooperativo hoy, usa
> [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop) (AGPL-3.0), que sí funciona con 2 jugadores.
> Hay un fork con arreglos extra en [zeroit789/KenshiCoop](https://github.com/zeroit789/KenshiCoop).
> Este repositorio se mantiene como referencia técnica: compila, pasa sus tests y su código está
> comentado en español e inglés, pero no se sigue desarrollando.

### Qué es

Kenshi es un RPG de mundo abierto para un solo jugador y no tiene multijugador oficial. Este proyecto
intenta añadirle cooperativo: varios jugadores en el mismo mundo, cada uno con su escuadra, conectados
a un servidor dedicado.

Es una continuación de [The404Studios/Kenshi-Online](https://github.com/The404Studios/Kenshi-Online)
(MIT). El código de junio de 2026 no compilaba (faltaban ficheros en los CMake); aquí se arregló la
compilación y después se trabajó sobre el arranque y la sincronización en la versión de Steam 1.0.68.

### En qué punto está (23-09-2026)

**Lo que funciona:**

- Compila entero (cliente, servidor, master server, inyector y herramientas de test) con
  CMake 4.4.3 y Visual Studio Build Tools 2022.
- Los 95 tests unitarios (`KenshiMP.UnitTest`) pasan.
- El test de integración (`KenshiMP.IntegrationTest`, servidor real + dos clientes falsos) pasa
  64 de 66 comprobaciones. Fallan dos: el cliente 1 no recibe el `EntityDespawn` cuando otro jugador
  se desconecta, y no recibe la confirmación de colocar un edificio.
- Dos jugadores reales, en dos PCs, llegaron a conectarse al servidor dedicado en junio de 2026. El
  servidor espera a que todos estén listos y guarda el mundo cada cierto tiempo.
- La escuadra de cada jugador nace en la facción nativa Nameless, así que los jugadores son aliados
  y no se atacan.
- Se arregló el cierre del juego al entrar el segundo jugador (las operaciones sobre el motor pasan
  por una cola que se ejecuta en el hilo del juego, `GameCommandQueue`).

**Lo que no funciona:**

- **Ver al otro jugador.** El personaje remoto se queda cargando: para crearlo hace falta la fábrica
  de objetos del juego (`RootObjectFactory`), que Kenshi solo rellena por una ruta que el flujo
  multijugador no usa. En julio se corrigió el offset de esa fábrica y se añadió un reintento, pero
  nunca se validó en una partida real con dos jugadores.
- **Combate.** El personaje del host nace sin la parte de IA (`char+0x650` a NULL), así que camina
  pero no combate. El arreglo existe en el código pero está desactivado porque la firma de la función
  que lo inicializa no está confirmada. La sincronización del combate no pasó de los hooks de
  diagnóstico.
- No hay reconciliación del lado del cliente.
- Los arreglos de julio (hostilidad espejo, armado del combate en frío, detección del clon,
  protección contra un use-after-free en la compra de objetos) compilan y se desplegaron, pero no se
  probaron en vivo.
- A la última prueba (junio de 2026): el servidor daba el mismo slot a los dos jugadores y la
  velocidad del juego no se sincronizaba entre PCs.
- Guardar y cargar partida no está enganchado: siempre hay que empezar con **New Game**.
- Solo se ha probado en Kenshi 1.0.68 de Steam. GOG no se ha probado.

### Requisitos

| Qué | Versión |
|-----|---------|
| Sistema | Windows 10/11 de 64 bits |
| Kenshi | 1.0.68 de Steam (`kenshi_x64.exe` de 36.718.592 bytes). Es la versión contra la que se sacaron los patrones y offsets |
| Cargador de mods | Ninguno. **No usa RE_Kenshi ni KenshiLib**: el DLL se carga como plugin de Ogre3D desde `Plugins_x64.cfg`. No se ha probado junto con RE_Kenshi |
| Compilador | Visual Studio 2022 o Build Tools 2022 con la carga «Desarrollo para el escritorio con C++» (MSVC v143 y Windows SDK), x64 |
| CMake | 3.20 o superior (probado con 4.4.3) |
| Opcional | .NET 10 SDK, solo para `tools/ModGen` (genera el `.mod`) |

Las dependencias de C++ vienen incluidas en `lib/`; no hace falta clonar con submódulos.

### Compilar

```bash
git clone https://github.com/zeroit789/kenshi-coop.git
cd kenshi-coop
cmake --preset x64-release
cmake --build build --config Release --parallel
```

Sin presets: `cmake -G "Visual Studio 17 2022" -A x64 -S . -B build`. Si compilas desde Git Bash,
usa `--parallel` y no `/m`: el MSYS de Git Bash convierte `/m` en una ruta.

Los binarios salen en `build/bin/Release/`. Si el repo está clonado dentro de la carpeta de Kenshi
(el padre del repo contiene `kenshi_x64.exe`), el build copia solo el DLL y los layouts al juego.

### Tests

```bash
build/bin/Release/KenshiMP.UnitTest.exe
build/bin/Release/KenshiMP.IntegrationTest.exe build/bin/Release/KenshiMP.Server.exe
```

- `KenshiMP.UnitTest`: 95 tests sin juego ni red. Compila dentro del test los mismos ficheros que
  van en el DLL (lectura de personajes, sondeo de offsets, registro de entidades, interpolación).
- `KenshiMP.IntegrationTest`: arranca un servidor real y dos clientes falsos y prueba el protocolo.
  Ojo: el servidor usa el `world.kmpsave` de la carpeta desde la que lo lanzas.
- `KenshiMP.TestClient`: jugador falso por consola (`KenshiMP.TestClient.exe [ip] [puerto] [nombre]`).
- `KenshiMP.LiveTest`: prueba con el juego real y un jugador falso. Necesita Kenshi instalado.

### Instalar

Opción A: `KenshiMP.Injector.exe`. Pide nombre y servidor, copia el DLL, añade la línea al
`Plugins_x64.cfg`, instala el `.mod` y abre el juego.

Opción B: `dist/install.bat` como administrador. Busca Kenshi en las rutas por defecto de Steam y
GOG, hace copia de lo que toca y lo instala.

Opción C, a mano, en la carpeta de Kenshi:

1. Copia `KenshiMP.Core.dll` junto a `kenshi_x64.exe`.
2. Añade `Plugin=KenshiMP.Core` al final de `Plugins_x64.cfg`.
3. Copia los tres `.layout` de `dist/` a `data/gui/layout/` (el del menú principal añade el botón
   Multiplayer).
4. Copia `kenshi-online.mod` a `mods/kenshi-online/` y añade `kenshi-online` a `data/__mods.list`.

Para desinstalar: `dist/uninstall.bat`.

### Jugar

1. Uno de los jugadores arranca `KenshiMP.Server.exe` (lee `server.json`; puerto UDP 27800). Si
   juegan desde fuera de la red local, hay que abrir ese puerto en el router (el servidor intenta
   UPnP).
2. **Todos entran por Join Game**, también quien tiene el servidor (con `127.0.0.1`). El botón Host
   abre otro servidor y choca con el puerto.
3. Menú principal → **Multiplayer** → **Join Game** → IP y puerto → **Connect**.
4. **New Game** (nunca Load Game) → elige el inicio **Multiplayer** → crea el personaje.

Teclas: **F1** menú multijugador, **Enter** chat, **Tab** lista de jugadores.

### Arquitectura

| Módulo | Qué es |
|--------|--------|
| `KenshiMP.Core` | El DLL cliente. Ogre lo carga como plugin y llama a `dllStartPlugin`. Contiene el singleton `Core` (ciclo de vida y tick), los hooks sobre funciones del juego (`hooks/`), los envoltorios de las estructuras de Kenshi y sus offsets (`game/`), el registro de entidades, la interpolación y la autoridad (`sync/`), el cliente ENet y el manejo de mensajes (`net/`), la UI nativa con MyGUI (`ui/`), los comandos de consola (`sys/`) y el estado del juego por sondeo (`sdk/`) |
| `KenshiMP.Scanner` | Resuelve en tiempo de ejecución las direcciones de funciones del juego: busca patrones de bytes (AOB), referencias a textos conocidos, la tabla `.pdata`, vtables y grafos de llamadas. También gestiona los hooks con MinHook |
| `KenshiMP.Common` | Lo que comparten cliente y servidor: tipos, constantes, protocolo, mensajes, serialización, compresión y configuración |
| `KenshiMP.Server` | Servidor dedicado con autoridad sobre el estado: jugadores, entidades, zonas de interés, validación de autoridad, resolución de combate, guardado del mundo en JSON y UPnP. Tick de 20 Hz |
| `KenshiMP.MasterServer` | Lista de servidores para el navegador de partidas (puerto 27801). No es necesario para jugar por IP |
| `KenshiMP.Injector` | Lanzador con ventana que instala el plugin y abre Kenshi |
| `KenshiMP.UnitTest`, `IntegrationTest`, `TestClient`, `LiveTest` | Tests y herramientas de prueba (ver arriba) |
| `tools/`, `_re-tools/` | Scripts de ingeniería inversa en Python sobre `kenshi_x64.exe`, el generador del `.mod` (`tools/ModGen`, C#) y el autotest |
| `docs/` | Documentación técnica e informes de ingeniería inversa (buena parte está en inglés y viene del proyecto original) |

Flujo: `kenshi_x64.exe` carga `KenshiMP.Core.dll` → el Scanner localiza las funciones → se instalan
los hooks → el tick del juego lee el estado local y lo manda al servidor por ENet (UDP) → el servidor
decide y reparte → cada cliente aplica e interpola lo que recibe.

### Licencia

[MIT](LICENSE), la misma licencia del proyecto original (The404Studios/Kenshi-Online), con su aviso
de copyright tal cual. Si haces un fork, conserva el `LICENSE` y los [créditos](CREDITS.md).

No incluye código de RE_Kenshi ni de KenshiLib (GPLv3): se usaron solo como referencia. La copia
local de KenshiLib que se usó para consultar está excluida del repo en `.gitignore`.

Las dependencias de `lib/` conservan sus licencias en su carpeta:

| Librería | Versión | Licencia | Uso |
|----------|---------|----------|-----|
| [ENet](https://github.com/lsalzman/enet) | 1.3.18 | MIT | Red (UDP fiable) |
| [MinHook](https://github.com/TsudaKageyu/minhook) | — | BSD-2-Clause | Hooks de funciones |
| [spdlog](https://github.com/gabime/spdlog) | 1.15.2 | MIT | Logs |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | Configuración y guardado |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.8 | MIT | Incluida pero sin usar: el overlay con ImGui se quitó por conflicto con Ogre3D/DX11 |

### Créditos

- [The404Studios/Kenshi-Online](https://github.com/The404Studios/Kenshi-Online) (MIT): la base
  directa de este código (arquitectura de red, plugin de Ogre, resolución de direcciones, protocolo,
  servidor y UI).
- [im-blatnoyua/kenshi-online-simplified](https://github.com/im-blatnoyua/kenshi-online-simplified)
  (MIT): referencia de zonas de interés, compresión delta e interpolación.
- [ernivani/kenshi-mp](https://github.com/ernivani/kenshi-mp): referencia para pasar la partida del
  host al que se une.
- [codiren/KServerMod](https://github.com/codiren/KServerMod): ideas de sincronización de posición y
  spawn.
- [BFrizzleFoShizzle/RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) y
  [KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) (GPLv3): referencia del mapa de memoria
  y de las estructuras del juego.
- [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop) (AGPL-3.0): el proyecto al que se pasó
  el esfuerzo; no hay código suyo aquí.
- La comunidad de Cheat Engine y de modding de Kenshi, por los offsets verificados.

Detalle completo en [CREDITS.md](CREDITS.md). Kenshi es de Lo-Fi Games; este proyecto no tiene
relación con ellos.

---

<a id="en"></a>

## English

> **Frozen since July 19, 2026. This is not a playable mod.**
> If you want to play Kenshi co-op today, use
> [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop) (AGPL-3.0), which works with 2 players.
> A fork with extra fixes lives at [zeroit789/KenshiCoop](https://github.com/zeroit789/KenshiCoop).
> This repository stays up as a technical reference: it builds, its tests pass, and the code is
> commented in Spanish and English, but it is no longer being developed.

### What it is

Kenshi is a single-player open-world RPG with no official multiplayer. This project tries to add
co-op: several players in the same world, each running their own squad, connected to a dedicated
server.

It continues [The404Studios/Kenshi-Online](https://github.com/The404Studios/Kenshi-Online) (MIT).
The June 2026 code didn't build (source files were missing from the CMake scripts). This repo fixed
the build and then worked on startup and sync against the Steam build 1.0.68.

### Where it stands (September 23, 2026)

**What works:**

- The whole tree builds (client, server, master server, injector and test tools) with CMake 4.4.3
  and Visual Studio Build Tools 2022.
- All 95 unit tests (`KenshiMP.UnitTest`) pass.
- The integration test (`KenshiMP.IntegrationTest`, a real server plus two fake clients) passes 64
  of 66 checks. Two fail: client 1 doesn't get an `EntityDespawn` when another player disconnects,
  and it doesn't get the confirmation for a placed building.
- Two real players on two PCs connected to the dedicated server in June 2026. The server waits
  until everyone is ready and saves the world periodically.
- Each player's squad spawns in the native Nameless faction, so players are allies and don't attack
  each other.
- The crash when the second player joined is fixed (engine operations go through a queue that runs
  on the game thread, `GameCommandQueue`).

**What doesn't work:**

- **Seeing the other player.** The remote character stays stuck loading. Creating it needs the
  game's object factory (`RootObjectFactory`), which Kenshi only fills in through a code path the
  multiplayer flow never takes. In July the factory offset was corrected and a retry was added, but
  it was never validated in a live two-player session.
- **Combat.** The host's character spawns without its AI part (`char+0x650` is NULL), so it walks
  but doesn't fight. The fix is in the code but disabled, because the signature of the function
  that initializes it isn't confirmed. Combat sync never got past diagnostic hooks.
- There's no client-side reconciliation.
- The July fixes (mirrored hostility, cold-start combat arming, clone detection, a use-after-free
  guard when buying items) build and were deployed, but were never tested live.
- As of the last test (June 2026), the server gave both players the same slot and game speed wasn't
  synced between PCs.
- Save/load isn't hooked: you always have to start with **New Game**.
- Only tested on Kenshi 1.0.68 from Steam. GOG is untested.

### Requirements

| What | Version |
|------|---------|
| OS | 64-bit Windows 10/11 |
| Kenshi | Steam 1.0.68 (`kenshi_x64.exe`, 36,718,592 bytes). The patterns and offsets were taken from this build |
| Mod loader | None. **It doesn't use RE_Kenshi or KenshiLib**: the DLL loads as an Ogre3D plugin from `Plugins_x64.cfg`. Running it alongside RE_Kenshi hasn't been tested |
| Compiler | Visual Studio 2022 or Build Tools 2022 with the "Desktop development with C++" workload (MSVC v143 and the Windows SDK), x64 |
| CMake | 3.20 or newer (tested with 4.4.3) |
| Optional | .NET 10 SDK, only for `tools/ModGen` (builds the `.mod`) |

The C++ dependencies are vendored in `lib/`, so there are no submodules to fetch.

### Build

```bash
git clone https://github.com/zeroit789/kenshi-coop.git
cd kenshi-coop
cmake --preset x64-release
cmake --build build --config Release --parallel
```

Without presets: `cmake -G "Visual Studio 17 2022" -A x64 -S . -B build`. If you build from Git
Bash, use `--parallel` rather than `/m`: Git Bash's MSYS layer turns `/m` into a path.

Binaries land in `build/bin/Release/`. If the repo is cloned inside the Kenshi folder (its parent
contains `kenshi_x64.exe`), the build copies the DLL and the layouts into the game automatically.

### Tests

```bash
build/bin/Release/KenshiMP.UnitTest.exe
build/bin/Release/KenshiMP.IntegrationTest.exe build/bin/Release/KenshiMP.Server.exe
```

- `KenshiMP.UnitTest`: 95 tests with no game and no network. It compiles the same files that ship in
  the DLL (character reads, offset probing, entity registry, interpolation).
- `KenshiMP.IntegrationTest`: starts a real server and two fake clients and exercises the protocol.
  Heads-up: the server uses the `world.kmpsave` in the folder you launch it from.
- `KenshiMP.TestClient`: a fake console player (`KenshiMP.TestClient.exe [ip] [port] [name]`).
- `KenshiMP.LiveTest`: a test with the real game plus a fake player. Needs Kenshi installed.

### Install

Option A: `KenshiMP.Injector.exe`. It asks for your name and the server, copies the DLL, adds the
line to `Plugins_x64.cfg`, installs the `.mod` and launches the game.

Option B: run `dist/install.bat` as administrator. It looks for Kenshi in the default Steam and GOG
paths, backs up what it changes and installs everything.

Option C, by hand, in the Kenshi folder:

1. Copy `KenshiMP.Core.dll` next to `kenshi_x64.exe`.
2. Append `Plugin=KenshiMP.Core` to `Plugins_x64.cfg`.
3. Copy the three `.layout` files from `dist/` into `data/gui/layout/` (the main menu one adds the
   Multiplayer button).
4. Copy `kenshi-online.mod` into `mods/kenshi-online/` and add `kenshi-online` to
   `data/__mods.list`.

To uninstall: `dist/uninstall.bat`.

### Play

1. One player runs `KenshiMP.Server.exe` (it reads `server.json`; UDP port 27800). For players
   outside your LAN, forward that port on the router (the server tries UPnP).
2. **Everyone joins through Join Game**, including whoever runs the server (using `127.0.0.1`). The
   Host button starts a second server and clashes over the port.
3. Main menu → **Multiplayer** → **Join Game** → IP and port → **Connect**.
4. **New Game** (never Load Game) → pick the **Multiplayer** start → create your character.

Keys: **F1** multiplayer menu, **Enter** chat, **Tab** player list.

### Architecture

| Module | What it is |
|--------|------------|
| `KenshiMP.Core` | The client DLL. Ogre loads it as a plugin and calls `dllStartPlugin`. It holds the `Core` singleton (lifecycle and tick), the hooks on game functions (`hooks/`), wrappers for Kenshi's structures and their offsets (`game/`), the entity registry, interpolation and authority (`sync/`), the ENet client and message handling (`net/`), the native MyGUI UI (`ui/`), console commands (`sys/`) and polled game state (`sdk/`) |
| `KenshiMP.Scanner` | Resolves game function addresses at runtime: it searches byte patterns (AOB), references to known strings, the `.pdata` table, vtables and call graphs. It also manages hooks through MinHook |
| `KenshiMP.Common` | Code shared by client and server: types, constants, protocol, messages, serialization, compression and config |
| `KenshiMP.Server` | Dedicated server that owns the state: players, entities, interest zones, authority checks, combat resolution, world saves as JSON and UPnP. Ticks at 20 Hz |
| `KenshiMP.MasterServer` | Server list for the game browser (port 27801). Not needed to play by IP |
| `KenshiMP.Injector` | Windowed launcher that installs the plugin and starts Kenshi |
| `KenshiMP.UnitTest`, `IntegrationTest`, `TestClient`, `LiveTest` | Tests and test tools (see above) |
| `tools/`, `_re-tools/` | Python reverse-engineering scripts for `kenshi_x64.exe`, the `.mod` generator (`tools/ModGen`, C#) and the autotest |
| `docs/` | Technical docs and reverse-engineering reports (much of it is in English and comes from the original project) |

Flow: `kenshi_x64.exe` loads `KenshiMP.Core.dll` → the Scanner finds the functions → the hooks go in
→ the game tick reads local state and sends it to the server over ENet (UDP) → the server decides
and broadcasts → each client applies and interpolates what it receives.

### License

[MIT](LICENSE), the same license as the original project (The404Studios/Kenshi-Online), with its
copyright notice kept as is. If you fork it, keep `LICENSE` and the [credits](CREDITS.md).

It contains no code from RE_Kenshi or KenshiLib (GPLv3); they were used only as references. The
local KenshiLib copy used for lookups is excluded from the repo in `.gitignore`.

The dependencies in `lib/` keep their licenses in their own folders:

| Library | Version | License | Used for |
|---------|---------|---------|----------|
| [ENet](https://github.com/lsalzman/enet) | 1.3.18 | MIT | Networking (reliable UDP) |
| [MinHook](https://github.com/TsudaKageyu/minhook) | — | BSD-2-Clause | Function hooks |
| [spdlog](https://github.com/gabime/spdlog) | 1.15.2 | MIT | Logging |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | Config and saves |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.8 | MIT | Vendored but unused: the ImGui overlay was removed because it clashed with Ogre3D/DX11 |

### Credits

- [The404Studios/Kenshi-Online](https://github.com/The404Studios/Kenshi-Online) (MIT): the direct
  base of this code (network architecture, Ogre plugin, address resolution, protocol, server and UI).
- [im-blatnoyua/kenshi-online-simplified](https://github.com/im-blatnoyua/kenshi-online-simplified)
  (MIT): reference for interest zones, delta compression and interpolation.
- [ernivani/kenshi-mp](https://github.com/ernivani/kenshi-mp): reference for sending the host's save
  to joining players.
- [codiren/KServerMod](https://github.com/codiren/KServerMod): ideas for position and spawn sync.
- [BFrizzleFoShizzle/RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) and
  [KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) (GPLv3): reference for the game's
  memory map and structures.
- [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop) (AGPL-3.0): where the effort moved to;
  none of its code is in this repo.
- The Cheat Engine and Kenshi modding communities, for verified offsets.

Full details in [CREDITS.md](CREDITS.md). Kenshi belongs to Lo-Fi Games; this project isn't
affiliated with them.
