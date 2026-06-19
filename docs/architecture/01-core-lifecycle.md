# 01 — Núcleo y ciclo de vida (`KenshiMP.Core/core.cpp` + `core.h`)

> Mapa de arquitectura del **corazón del mod**: la clase `Core` (singleton) que orquesta
> todo el ciclo de vida de Kenshi Co-op, desde que la DLL se inyecta hasta el bucle de
> sincronización por frame.
>
> Ámbito: `KenshiMP.Core/core.cpp` (4034 líneas) y `core.h` (361 líneas).
> Namespace: `kmp`. Fuente verificada al detalle; los offsets de campo (`+0x...`) y RVAs
> citados provienen literalmente del código.

---

## 0. Índice rápido

| Sección | Qué cubre |
|---|---|
| [1](#1-la-clase-core-singleton) | La clase `Core`, singleton, miembros clave |
| [2](#2-máquina-de-estados-de-fases-clientphase) | `ClientPhase`: Startup → MainMenu → Loading → GameReady → Connecting → Connected |
| [3](#3-arranque-coreinitialize) | `Initialize()`: logging, VEH, scanner, hooks, red, orquestadores |
| [4](#4-detección-de-carga-pollforgameload--onloadinggapdetected) | Detección de carga de partida |
| [5](#5-coreongameloaded) | `OnGameLoaded()`: el "world ready" |
| [6](#6-conexión-y-handshake-flujo-externo) | Handshake (en `packet_handler.cpp`) y resume de hooks |
| [7](#7-coreongametick--el-bucle-por-frame) | `OnGameTick()` paso a paso (force-unpause, fix facción, etc.) |
| [8](#8-flujo-de-entidades) | `FindAndClaimModCharacters` / `SendExistingEntitiesToServer` / `HandleSpawnQueue` |
| [9](#9-fix-de-facción-del-host-detalle) | El fix de facción del host (causa raíz del "combate roto") |
| [10](#10-desconexión-y-shutdown) | `SetConnected(false)` y `Shutdown()` |
| [11](#11-diagnóstico-de-crashes) | VEH, breadcrumbs, SEH wrappers |
| [12](#12-estadobugs-conocidos-resumen) | Tabla resumen de estado/bugs |

---

## 1. La clase `Core` (singleton)

`Core` es un **singleton Meyers** (`Core::Get()` → `static Core instance`, línea 322). Posee
TODOS los subsistemas por valor o `unique_ptr`. No hay estado global disperso: casi todo cuelga
de aquí.

### Subsistemas que posee (`core.h` 294-357)

| Miembro | Tipo | Rol |
|---|---|---|
| `m_scanner` | `PatternScanner` | Scanner legacy (AOB / RVA) |
| `m_gameFuncs` | `GameFunctions` | Punteros a funciones del motor + singletons (`PlayerBase`, `GameWorldSingleton`) |
| `m_patternOrchestrator` | `PatternOrchestrator` | Pipeline de 7 fases de descubrimiento de patrones |
| `m_client` | `NetworkClient` | Cliente ENet |
| `m_entityRegistry` | `EntityRegistry` | Mapa netId ↔ gameObject + metadatos por entidad |
| `m_interpolation` | `Interpolation` | Buffer de interpolación de posiciones remotas |
| `m_spawnManager` | `SpawnManager` | Cola de spawns, factory, plantillas mod |
| `m_playerController` | `PlayerController` | Facción local, personaje primario, remotos |
| `m_loadingOrch` | `LoadingOrchestrator` | Gating de spawns por recursos (assets cargados) |
| `m_lobbyManager` | `LobbyManager` | Slot de jugador, facción asignada, parche de facción |
| `m_overlay` / `m_nativeHud` | UI | Overlay MyGUI + HUD nativo de logs |
| `m_syncOrchestrator` | `unique_ptr<SyncOrchestrator>` | Pipeline nuevo de 7 etapas (EntityResolver/ZoneEngine/PlayerEngine) |
| `m_pipelineOrch` | `PipelineOrchestrator` | Debugger de pipeline replicado por red |
| `m_orchestrator` | `TaskOrchestrator` | Pool de hilos (2 workers) para trabajo de fondo |
| `m_frameData[2]` | `FrameData` | Doble-buffer para el pipeline legacy (read/write) |
| `m_sdk` / `m_visualProxy` | `KenshiSDK` / `VisualProxy` | Polling de estado + proxies visuales de remotos |
| `m_commandQueue` | `GameCommandQueue` | Marshalling thread-safe red→hilo de juego |

### Flags atómicos de estado (`core.h` 308-328)

- `m_running`, `m_connected`, `m_gameLoaded` — `std::atomic<bool>`
- `m_clientPhase` — `std::atomic<ClientPhase>` (la verdad sobre el estado del cliente)
- `m_isHost` / `m_hostPlayerId` — autoridad de host (server-authoritative)
- `m_localPlayerId` — `std::atomic<PlayerID>`
- `m_useSyncOrchestrator` — **feature flag** que decide qué rama del pipeline corre en `OnGameTick`

### Predicados clave

- **`IsHost()`** (`core.h` 89) → `m_isHost`. Se fija vía `SetLocalHostPlayerId()` (210-... en .h):
  `m_isHost = (id != 0 && id == m_localPlayerId)`. Es decir, eres host si el host-id asignado por
  el servidor coincide con tu propio playerId. Usado en `OnGameTick` para disparar el fix de facción.
- **`IsConnected()`** (`core.h` 88) → `m_connected`. Gate principal de `OnGameTick` (sale temprano si no).
- **`IsGameLoaded()`** → `m_gameLoaded`. Gate del pipeline de mundo.
- **`IsLoading()`** → `m_clientPhase == Loading`.

### Hilos involucrados

1. **Hilo de render** (Present hook de D3D11) → llama `PollForGameLoad()` y, como fallback, `OnGameTick()`.
2. **Hilo de juego / lógica** (TimeUpdate hook) → driver principal de `OnGameTick()`.
3. **Hilo de red** (`m_networkThread`, función `NetworkThreadFunc`, línea 1852) → `m_client.Update()` en bucle, protegido por SEH.
4. **Workers del orquestador** (2) → `BackgroundReadEntities()` / `BackgroundInterpolate()` (solo rama legacy).

> ⚠ **Thread-safety:** las escrituras a memoria del juego desde el hilo de red SIEMPRE pasan por
> `m_commandQueue`, que se drena en `OnGameTick` (Step 1.5) sobre el hilo de juego/OGRE. Ver línea 2486.

---

## 2. Máquina de estados de fases (`ClientPhase`)

Definida en `core.h` 37-58. Es una **máquina de estados determinista**; las transiciones ocurren en
sitios concretos vía `Core::TransitionTo()` (línea 1253).

```
Startup ──► MainMenu ──► Loading ──► GameReady ──► Connecting ──► Connected
   │           │            ▲            │  ▲            │              │
   │           │            │            │  │            │              │
 (DLL load) (splash    (clic New/    (OnGameLoaded) (auto-connect)  (handshake
  scan+hook  done,      Continue,                                    ACK en
  install)   Present     gap >2s)                                    packet_handler)
             a buen fps)
```

| Fase | Significado | Quién entra aquí |
|---|---|---|
| `Startup` | DLL cargada, scan + install de hooks. Present aún no dispara. | Estado inicial |
| `MainMenu` | Splash hecho, usuario en menú. Present a fps alto. | render_hooks al detectar fps estable |
| `Loading` | Usuario pulsó New/Continue/Load. Juego bloqueado cargando. **Detectado por gap >2s entre Present.** | `OnLoadingGapDetected()` (1270) |
| `GameReady` | Mundo cargado, personajes existen. `OnGameLoaded()` ha disparado. Auto-connect puede proceder. Hook CharacterCreate aún en modo seguro. | `OnGameLoaded()` (1465) |
| `Connecting` | `ConnectAsync` llamado, esperando handshake. | `OnGameLoaded()` (1706) o comando manual |
| `Connected` | Handshake hecho, CharacterCreate habilitado, entidades sincronizando. | `packet_handler.cpp:361` |

### `TransitionTo()` — efecto colateral importante (1253-1268)

Al entrar/salir de `Loading`, hace de **puente con el guard de carga** de `CharacterIterator`:

```cpp
if (newPhase == Loading)      game::SetGameLoadingState(true);
else if (old == Loading)      game::SetGameLoadingState(false);
```

Esto evita que `CharacterIterator` lea la "lektor" (lista de personajes) mientras el juego la
está redimensionando → corrupción de heap.

---

## 3. Arranque: `Core::Initialize()`

Línea **327-595**. Es el `DllMain`-equivalente lógico. Orden EXACTO:

1. **Logging spdlog** → fichero `KenshiOnline_<PID>.log` (PID para no pisarse entre instancias).
2. **Captura rango del módulo del juego** (`g_gameModuleBase/End`) leyendo el PE header (SizeOfImage). Necesario para el filtrado del VEH.
3. **Registra VEH** (`AddVectoredExceptionHandler(1, VectoredCrashHandler)`, 355) — dispara ANTES que el SEH de Kenshi (que sobreescribe `SetUnhandledExceptionFilter`).
4. **Cabecera de sesión** en `KenshiOnline_CRASH.log` + **fichero breadcrumb** (`...\Kenshi\KenshiOnline_BREADCRUMB.txt`).
5. **atexit + SIGABRT + SetUnhandledExceptionFilter** — red de última instancia para el "crash silencioso post-loading" que no es AV.
6. **Detección Steam vs GOG** (464-482): comprueba `steam_api64.dll` cargada → `m_isSteamVersion`. También loguea tamaño del exe como huella de versión.
7. **Config** (`m_config.Load`).
8. **Offsets** — `game::InitOffsetsFromScanner()` (fallbacks CE) + `game::LoadOffsetCache()` (cache runtime).
9. **`InitScanner()`** (727) — ver abajo.
10. **MinHook** (`HookManager::Get().Initialize()`) — si falla, `return false` (abortar).
11. **`InitHooks()`** (923) — instala detours.
12. **`InitNetwork()`** (1058) — `m_client.Initialize()` (ENet).
13. **`InitPacketHandler()`** — registra handlers de mensajes.
14. **Callback de spawn** (542-553): cuando `SpawnManager` materializa una entidad, enlaza netId→gameObject, notifica `PlayerController`, lee nombre y muestra mensaje.
15. **`InitUI()`** — overlay diferido (init lazy cuando D3D11 device esté listo).
16. **`m_running = true`**, comandos slash, **arranca orquestador (2 workers)**.
17. **Construye `m_syncOrchestrator`** (571) y bindea facilitadores (`SyncFacilitator`, `AssetFacilitator`), inicializa `m_pipelineOrch`. `m_useSyncOrchestrator = m_config.useSyncOrchestrator`.
18. **Arranca hilo de red** (`m_networkThread = thread(&Core::NetworkThreadFunc)`).

### `InitScanner()` (727-918) — descubrimiento de funciones

- Scanner **legacy** (`m_scanner.Init`) + **PatternOrchestrator** de 7 fases (749-774):
  1. enumeración `.pdata`, 2. strings+xref, 3. vtables+RTTI, 4. SIMD batch scan,
  5. xref fallback, 6. call-graph+label propagation, 7. validación de punteros globales.
- **Validación de singletons** (784-843): los RVAs hardcoded `PlayerBase=0x01AC8A90`,
  `GameWorld=0x02133040` son **solo GOG 1.0.68**. En Steam contienen basura. `validateSingleton`
  comprueba que el valor sea un puntero de heap real fuera del módulo; si no, **limpia a 0**.
  - ⚠ **GameWorld 1.0.68 = instancia embebida**, no puntero. Por eso usa
    `ValidateGameWorldGlobal()` (56-72) que acepta AMBOS layouts: (a) puntero clásico a objeto
    con vtable en `.text`, o (b) instancia directa cuyo primer qword YA es la vtable en `.text`.
- **Análisis de firmas** (845-902): `FunctionAnalyzer::Analyze` valida prólogos de funciones
  hookeables; si la firma no cuadra con el número de params esperado, NULea las peligrosas
  (`CharacterMoveTo`, `BuildingPlace`, `SaveGame`, `LoadGame`). Las `MovRaxRsp` confunden al
  analizador → solo warning, no se desactivan.
- Puentea `PlayerBase` y `CharacterSetPosition` al módulo `game_character`.

### `InitHooks()` (923-1056) — qué se instala y cuándo

| Hook | Cuándo | Nota |
|---|---|---|
| **D3D11 Present** (`render_hooks`) | Siempre | WndProc (input) + tick de frame fallback. NO renderiza ImGui (conflicto Ogre/DX11). UI es MyGUI nativa. |
| **CharacterCreate** (`entity_hooks`) | Instalado **pero DISABLED** | Crítico: los 130+ creates de carga por el detour naked `MovRaxRsp` corrompen el heap. Solo se habilita al conectar (`ResumeForNetwork`). |
| **Combat** (`combat_hooks`: ApplyDamage, Death, KO) | Early | Disparan por evento individual, el wrapper MovRaxRsp los tolera. SEH + gate `IsConnected`. |
| **Squad spawn bypass** | Early | Para spawn fiable de remotos. |
| **Char tracker** | Early | Hook de update de animación, trackea todos los chars por nombre. |
| **Inventory / Faction / Time / AI** | Early | Cada uno con su gate. **Time hooks → drive de `OnGameTick`** (esencial). |

> Los guards de carga (`SetLoading(true)`) se ponen en inventory/faction para que no manden
> paquetes durante el load del save.

---

## 4. Detección de carga: `PollForGameLoad` + `OnLoadingGapDetected`

### `OnLoadingGapDetected()` (1270-1321)
Llamado por `HookPresent` cuando detecta un **gap >2s** entre frames (juego bloqueado cargando).
Acepta solo desde `MainMenu` (flujo normal) o `GameReady` (botón Load in-game, gap >10s). **No**
desde `Startup` (los gaps de init del motor no son loads).

Hace:
1. Resetea `m_gameLoaded`, `m_initialEntityScanDone`, `m_spawnTeleportDone` para que `OnGameLoaded`
   pueda volver a disparar en el nuevo save.
2. Si venía de `GameReady` (load in-game): **limpia TODO el estado stale** (registry, interpolación,
   playerController, frameData, probe state) para evitar UAF sobre objetos liberados. Re-arma el fix
   de facción del host (`ResetHostFactionFix()`).
3. Aplica el **parche de string de facción** ANTES de cargar (`m_lobbyManager.ApplyFactionPatch()`)
   — determina qué facción controlas al cargar el save (parchea el string de 17 bytes en `.rdata`).
4. Activa **loading passthrough** en CharacterCreate (camino ultra-ligero: timestamp + contador,
   sin tocar estructuras del juego).
5. `TransitionTo(Loading)`.

### `PollForGameLoad()` (1337-1455)
Llamado desde el hilo de render. Detecta cuándo terminó la carga. Lógica:
- Reintenta `RetryGlobalDiscovery` (resolver PlayerBase/GameWorld ahora que el juego cargó).
- **Detección por eventos de create** (NO por CharacterIterator, que corrompería el heap):
  - `loadingCreates > 0` → personajes creándose (carga empezó)
  - `timeSinceLastCreate > 3000ms` → los creates pararon (ráfaga de carga terminó)
  - AMBOS → mundo cargado → `OnGameLoaded()`.
- **Fallbacks anti-deadlock** (críticos en Steam donde GameWorld no resuelve):
  - 30 polls + globals válidos → intenta `CharacterIterator` (ya estable post-load).
  - 60 polls (~120s) → "ultimate fallback", asume cargado.
  - **45 polls (~90s) → HARD TIMEOUT incondicional** → fuerza `OnGameLoaded` (previene deadlock Steam).

---

## 5. `Core::OnGameLoaded()`

Línea **1461-1840**. Idempotente: `if (m_gameLoaded.exchange(true)) return;`. El "mundo está listo".
Secuencia:

1. `TransitionTo(GameReady)`.
2. **Limpia guards de carga** en todos los hooks (building/inventory/squad/faction) + `m_loadingOrch.OnGameLoaded()`.
3. **Deferred network resume** (1482): si ya estaba conectado (conectó desde menú), ahora habilita
   `entity_hooks::ResumeForNetwork()` + combat hooks + `StartAttack` (DIAG). Antes estaba diferido
   para no habilitar CharacterCreate durante los 130+ creates de carga.
4. **Instala hooks post-load**: movement (null en Steam → polling de posición), squad, resource (vtable Ogre).
5. **Descubrimiento global diferido de PlayerBase/GameWorld** (1531-1591) con re-validación
   (instancia-embebida vía `ValidateGameWorldGlobal`). Pone los puentes `SetResolvedPlayerBase` /
   `SetResolvedGameWorld` para `CharacterIterator`.
6. `PlayerController::OnGameWorldLoaded()` (SEH-protegido).
7. **Descubrimiento de `SquadAddMember` por vtable** si el scan falló (`TryDiscoverSquadAddMemberFromVTable`).
8. **Verifica spawn system** + heap scan temprano (`SEH_ScanGameDataHeap`) + `FindModTemplates`.
9. **Desactiva loading passthrough** → CharacterCreate corre cuerpo completo (spawns runtime van por el hook completo).
10. **Deferred mod link** (1668): si conectó antes de cargar, enlaza entidades remotas pendientes a
    sus "Player N" mediante `FindModCharacterBySlot`.
11. **AUTO-CONNECT** (1699): si no conectado y `m_config.autoConnect` → `ConnectAsync` → `Connecting`.
    > Crítico: se conecta DESPUÉS de cargar, nunca en menú.
12. **Handshake de multiplayer** (1716): si conectado, manda `C2S_PlayerReady`.
13. **Dump completo de funciones + offsets** al log (1727-1825) — referencia de RE en runtime.
14. Inicializa `m_sdk` + `m_visualProxy`.

---

## 6. Conexión y handshake (flujo externo)

La transición final a **`Connected`** NO está en `core.cpp`, sino en `net/packet_handler.cpp:361`
(`core.TransitionTo(ClientPhase::Connected)`), tras el ACK del handshake. Allí también se llama a
`entity_hooks::ResumeForNetwork()` (385) y, en otros puntos, a `Core::SendExistingEntitiesToServer()`.

**`Core::SetConnected(bool)`** (`core.h` 105-191) es el punto único de entrada/salida de conexión:
- **Al conectar:** resetea keepalive; si `m_gameLoaded`, habilita combat hooks + `StartAttack` (DIAG);
  si no, los difiere a `OnGameLoaded`.
- **Al desconectar:** ver [sección 10](#10-desconexión-y-shutdown).

`SendExistingEntitiesToServer` / `FindAndClaimModCharacters` se invocan desde:
- `OnGameTick` Step 1 (escaneo inicial con reintentos),
- `packet_handler.cpp` y `builtin_commands.cpp` (comandos manuales `/sync`, `/claim`).

---

## 7. `Core::OnGameTick()` — el bucle por frame

Línea **2256-2726**. Driver: **TimeUpdate hook** (principal) + **Present hook** (redundancia).
Cada "Step N" actualiza `g_lastTickStep`/`g_lastStepName` + breadcrumb para que el VEH reporte
dónde crasheó. Secuencia:

### Gates de entrada (2257-2318)
1. **Pre-check diagnostics** (contador).
2. **`if (!m_connected) return;`** — gate principal.
3. **Dedup por frame** (2274): como lo llaman DOS hooks, exige ≥4ms entre ticks (máx ~166fps) para
   que el pipeline no se ejecute doble (el swap de buffers NO es idempotente).
4. **Keepalive** (2288): cada 5s manda `C2S_Keepalive` (timeout del server = 10s). ANTES del gate de
   carga, así dispara incluso durante loading.
5. **Game-loaded gate** (2305): si `!m_gameLoaded`, intenta detectar carga vía PlayerBase fallback
   (para connect-before-load) y `return`. Sin esto, `HandleSpawnQueue` spawnearía durante la carga → crash.

### Step 0 — Force unpause (2338-2390) ⚠ CRÍTICO
Kenshi permite pausar (Space) congelando el mundo. En multiplayer hay que impedirlo (el server sigue
ticando). Cada tick:
- `GameWorldAccessor world(game::GetResolvedGameWorld())`.
- Lee `paused` (offset **+0x8B9**) y `gameSpeed` (offset **+0x700**). Si `paused != 0` (1 o -1
  desconocido) → `world.SetPaused(false)` (idempotente y barato).
- Fuerza `gameSpeed` a 1.0 si está fuera de `[0.5, 3.5]` (necesario: delta_sim = frameTime × gameSpeed).

> **BUG HISTÓRICO ARREGLADO (causa raíz del "combate no funciona"):** el código antiguo hacía
> `gwPtr = *gwSingleton` (deref ciega) y escribía en `gwPtr + 0x8B9`. En Steam 1.0.68 GameWorld es
> instancia embebida, así que `*gwSingleton` devuelve la **vtable** (en `.text`), y `vtable+0x8B9`
> cae en página de SOLO LECTURA → la escritura `paused=0` fallaba en silencio → cliente quedaba
> PAUSADO → IA/combate/movimiento nunca avanzaban aunque el server mandara speed=1. **Fix:**
> `GameWorldAccessor` resuelve el objeto real vía `ResolveWorldObject()` (maneja instancia-embebida
> Y puntero). Ver comentario 2343-2350.

### Step 0.5 — Cleanup de snapshots pendientes (2392)
Cada 300 ticks, `PendingSnapshotQueue::CleanupOld` purga updates >10s de entidades que nunca spawnearon.

### Step 1 — Escaneo de entidades con reintentos (2402-2456)
Si `!m_initialEntityScanDone`: cada 150 ticks (~1s), hasta 45 reintentos (~30s):
1. **`FindAndClaimModCharacters()`** primero (encuentra "Player N" por nombre — lo más fiable),
   si `m_lobbyManager.HasFaction()` y slot > 0.
2. Fallback: **`SendExistingEntitiesToServer()`** (escaneo por facción) si no hay entidades locales.
- Cuando hay ≥1 local → `m_initialEntityScanDone = true`.

### Step 1b — Re-scan tras bootstrap de facción (2462)
Si `m_needsEntityRescan` (puesto por entity_hooks tras descubrir la facción del primer char) →
`SendExistingEntitiesToServer()` de nuevo.

### Step 1c — Descubrimiento vtable de SquadAddMember (2475)
Reintenta cada 100 ticks (~21s) si el patrón falló.

### Step 1.5 — Drenar command queue (2486-2501) ⚠ thread-safety
**TODAS las escrituras red→memoria del juego se ejecutan aquí**, en el hilo de juego/OGRE.
`m_commandQueue.DrainAll(...)`.

### Steps 2-9 — Pipeline de sync (DOS ramas)
Bifurca por `m_useSyncOrchestrator`:

**Rama NUEVA (sync orchestrator, 2504-2563):**
- Step 2: `SEH_InterpolationUpdate`
- Step 3: `SEH_SyncOrchestratorTick` (el grueso: EntityResolver/ZoneEngine/PlayerEngine)
- Step 4: `SEH_LoadingOrchTick`
- Step 5: `HandleSpawnQueue` + probes diferidos (`ProcessDeferredAnimClassProbes`, `DiagTickPump`)
- **`if (IsHost()) FixHostCharacterFactionTick(*this);`** ← fix de facción del host
- Offset prober, eventos diferidos (combat/char-tracker/zone), `m_sdk.Update`, `m_visualProxy.Update`, `shared_save_sync::Update`
- Step 6: `HandleHostTeleport`

**Rama LEGACY (doble-buffer, 2564-2659):**
- Step 2: `WaitForFrameWork` (espera a los workers)
- Step 3: **swap de buffers** (read↔write) — por esto el dedup de 4ms es crítico
- Step 4: interpolación; Step 5: `ApplyRemotePositionsDirect`; Step 6: `PollLocalPositions`
- Step 8: loading orch; Step 9: `HandleSpawnQueue` + mismos probes/eventos que la rama nueva
  (incluido **`FixHostCharacterFactionTick`**)
- Step 10: `HandleHostTeleport`; Step 11: `KickBackgroundWork` (re-lanza workers).

### Step 9c — Validación periódica de facción remota (2664-2710)
Cada 50 ticks (~0.5s): el juego lee `faction+0x250` en CADA update de char (game+0x927E94). Si la
facción de un remoto fue liberada (zone unload) → crash. Repara punteros stale vía
`SEH_ValidateEntityFaction` (solo si están **realmente** inválidos; NO sobreescribe facciones válidas
porque los remotos deben estar en facción DISTINTA para que PvP/robo funcionen).

### Step 10-11 — Diagnostics + pipeline debugger (2712-2725)
`UpdateDiagnostics` (log de ticks/s cada 5s) + `m_pipelineOrch.Tick`.

---

## 8. Flujo de entidades

### `SendExistingEntitiesToServer()` (1876-2010)
Tras conectar: escanea personajes existentes y los registra+envía. **Solo de la facción del jugador**
(miembros del escuadrón).
- **Fuente primaria de facción:** `game::GetPlayerFactionDirect()` — getter directo
  `GameWorld → player(+0x580) → participant(+0x2A0)`. No depende de la lista de personajes (resuelve
  el bug `faction=0x0` cuando la lista aún no está poblada).
- Fallback: `m_playerController.GetLocalFactionPtr()`; si sigue 0, `entity_hooks::RevalidateFaction()`
  (voting multi-fuente: nombre + flag isPlayerFaction + frecuencia).
- Itera `CharacterIterator`: por cada char de la facción del jugador → `Register(NPC)` → escribe
  pos/rot/facción/nombre + **estado extendido** (salud de 7 limbs + flag alive) → `C2S_EntitySpawnReq` reliable.
- Como efecto secundario, descubre el offset `isPlayerControlled` comparando un char player vs un NPC
  (`game::ProbePlayerControlledOffset`).

### `FindAndClaimModCharacters()` (2036-2149)
La estrategia de spawn **preferida y más fiable**. La mod `kenshi-online.mod` ya creó "Player 1"…
"Player 16" en el mundo. En vez de llamar a `FactoryCreate` (poco fiable), los encuentra por nombre:
- Itera chars, parsea "Player N" (N = slot 1..16).
- **Tu slot (`mySlot`)** → registra como **`PlayerCharacter` LOCAL**; lo desplaza en círculo de 3m
  según slot para no spawnear unos dentro de otros; lo manda al server con estado extendido.
- **Otros slots** → quedan disponibles; el registro remoto real ocurre cuando el server manda
  `S2C_EntitySpawn` y `HandleEntitySpawn` los enlaza.

### `FindModCharacterBySlot(int slot)` (2153-2170)
Busca por nombre exacto "Player N" y devuelve el gameObject o nullptr. Usado por el deferred-link y
por `HandleSpawnQueue` (PATH 0).

### `HandleSpawnQueue()` (3309-3708)
El materializador de remotos. Gating fuerte:
- `if (!m_gameLoaded) return;` y `AssetFacilitator::Get().CanSpawn()` (gating por recursos).
  **Bypass de playtest:** si lleva ≥8s bloqueado, fuerza el paso.
- Heap scan con reintentos (≤5, cooldown 5s) para encontrar plantillas mod.
- **Spawn directo** (tras 2s pendiente, throttle 1s), 1 request por tick, **cap de 1 char por jugador**
  (evita inundar el panel de escuadrón):
  - **PATH 0** — enlazar "Player N" existente (`FindModCharacterBySlot`, retry en hilo de juego donde
    CharacterIterator es fiable) → `SetGameObject` + `MarkRemoteControlled` + `SEH_FallbackPostSpawnSetup` +
    `WriteGameDataNameForModLink` + `SEH_AllyModFaction`.
  - **PATH 1** — plantilla mod (`SpawnCharacterDirect`, apariencia correcta).
  - **PATH 2** — `CallFactoryCreateRandom` (fallback, apariencia incorrecta pero funcional). Solo a
    los random se les aplica `SEH_FixUpFaction_Core`.
- Limpieza de entidades stuck (cada 10s: remotos en `Spawning` sin gameObject y cola vacía → unregister).

### Pipeline de posición
- **`PollLocalPositions()`** (3162): lee pos/rot de TUS chars (SEH), manda `C2S_PositionUpdate`
  unreliable con animState/moveSpeed/flags derivados del delta.
- **`ApplyRemotePositionsDirect()`** (3085): lee interpolación y escribe en los chars remotos vía
  `SEH_WritePositionRotation` (valida quaternion: NaN/Inf + magnitud; no pisa +0x58 si parece puntero/SceneNode).
- **`BackgroundReadEntities` / `BackgroundInterpolate`** (3769 / 3869): versión multihilo de lo anterior
  (rama legacy), llena `m_frameData[writeBuffer]`.
- **`HandleHostTeleport()`** (3254): el joiner espera 2s y teletransporta su escuadrón al spawn del host.
- **`TeleportToNearestRemotePlayer()`** (3942) y **`ForceSpawnRemotePlayers()`** (3904): escotillas para comandos `/tp` y `/forcespawn`.

---

## 9. Fix de facción del host (detalle)

Estado one-shot re-armable (2188-2197): `s_hostFactionFixed`, `s_hostFactionAttempts`
(máx 120), throttle 250ms. `ResetHostFactionFix()` lo re-arma en disconnect / nueva carga.

**`FixHostCharacterFactionTick(Core&)`** (2203-2254): llamado desde AMBAS ramas de `OnGameTick`,
**solo si `IsHost()`**:
1. Resuelve la PLAYER faction: `game::GetPlayerFactionDirect()` (fuente de verdad) →
   fallback `GetLocalFactionPtr()`.
2. Itera los GameObjects locales del host (`GetPlayerEntities(localId)`) y, en los que tengan
   `faction != player` (offset **char+0x10**), escribe la player faction válida vía
   `game::FixCharacterFactionTo(gameObj, playerFaction)`.
3. One-shot cuando al menos uno queda `Fixed`/`AlreadyCorrect`.

**Por qué importa:** sin esto, el char del host queda con facción NULL/incorrecta → el motor no te
reconoce como jugador y rechaza tus órdenes de combate. Es el complemento del force-unpause para
arreglar "el combate no funciona".

> Contraste con remotos: a los remotos NO se les fuerza la facción del jugador (`SEH_ValidateEntityFaction`,
> `SEH_FixUpFaction_Core` solo reparan punteros stale/freed). Deben estar en facción distinta para que
> PvP y robo funcionen.

---

## 10. Desconexión y shutdown

### `SetConnected(false)` (`core.h` 130-191)
- Vuelve a `GameReady` (si game loaded) para permitir reconectar.
- `entity_hooks::SuspendForDisconnect()` (desactiva CharacterCreate; los bursts de zone-load por el
  detour MovRaxRsp corromperían el heap estando desconectado).
- Desactiva combat hooks.
- Limpia remotos, interpolación, visual proxies, `PlayerController.Reset()`.
- Resetea todo el estado de sesión (host flags, frameData, sync orchestrator, shared_save_sync,
  cola de spawn) y marca flags de reset diferido (`m_needSpawnQueueReset`, etc.).
- `game::ResetProbeState()` (re-descubrir animClassOffset en la próxima conexión).
- Cierra chat input, resetea auto-connect del overlay.

### `Shutdown()` (677-725)
Cada paso es **SEH-protegido individualmente** (los widgets MyGUI pueden estar ya liberados por
Kenshi → AV en teardown). Para hilo de red, desinstala hooks inline (squad_spawn, char_tracker),
NativeHud/Overlay/HookManager bajo SEH, quita el VEH, guarda config en path por-instancia.

---

## 11. Diagnóstico de crashes

Sistema robusto, varios niveles (todo en `core.cpp` cabecera 74-320):

- **VEH (`VectoredCrashHandler`, 126-320):** dispara antes que el SEH de Kenshi. Filtra por RIP
  (solo loguea crashes en: módulo del juego, nuestra DLL, hook stubs VirtualAlloc'd, NULL, Ogre/MyGUI;
  **excluye PhysX** que cae en el rango de stubs). Salta AVs benignos de `Memory::Read`/SEH-helpers por
  rango de RVA (dll+0x1A00..0x1D00, +0x2BD00..0x2D000, +0x21000..0x22200) y durante loading (tick=0).
  Dedup por RIP (máx 3) + rate-limit (20/sesión). Logging ligero SIN spdlog (evita deadlock de mutex):
  `OutputDebugStringA` + escritura C directa a `KenshiOnline_CRASH.log` con dump de 16 registros + stack.
- **Breadcrumbs (`WriteBreadcrumb`, 94-108):** escribe el último step a un fichero ANTES de cada
  operación peligrosa, con fflush. Sobrevive a `__fastfail`/`TerminateProcess`.
- **Breadcrumb de step en tiempo real:** `g_lastTickStep` / `g_lastStepName` / `g_tickNumber` /
  `g_lastCharacterCreateNum` — globales `volatile` que el VEH reporta.
- **atexit + SIGABRT + UnhandledExceptionFilter:** para el "crash silencioso post-loading" que no es AV.
- **Wrappers SEH** por todas partes (`SEH_*`): regla estricta = **solo tipos POD/trivialmente
  destructibles dentro de `__try`** (MSVC no permite objetos con destructor). De ahí que muchas
  funciones helper estén extraídas a funciones libres con sufijo `SEH_`.

---

## 12. Estado/bugs conocidos (resumen)

| Área | Estado | Nota |
|---|---|---|
| Conexión + creación de personaje (cualquier raza) | ✅ FUNCIONA | Confirmado en memoria del proyecto |
| Force-unpause (Step 0) | ✅ ARREGLADO | Era la causa raíz del "combate no funciona"; usaba deref ciega de GameWorld embebido |
| Fix de facción del host (Step "FixHostCharacterFactionTick") | ✅ Implementado | One-shot, throttled, solo host; escribe char+0x10 |
| Facciones / enemigos huyen | 🟡 PENDIENTE | Bug abierto: los remotos NO deben compartir facción del jugador (PvP/robo) pero el balance fino sigue en ajuste |
| Steam: singletons hardcoded (PlayerBase/GameWorld) | ⚠ Frágiles por versión | RVAs `0x01AC8A90`/`0x02133040` son **solo GOG 1.0.68**; en Steam se limpian y se usa fallback de entity_hooks + cache. GameWorld = instancia embebida |
| Movement hook | ⚠ Null en Steam | Se usa polling de posición en su lugar |
| Squad injection en post-spawn | ❌ DESACTIVADO | `activePlatoon` recoge punteros de `.text` en Steam → WRITE AV en game+0xE85340 |
| Detección de carga en Steam | ⚠ Mitigado | GameWorld no resuelve → HARD TIMEOUT 90s incondicional en `PollForGameLoad` para evitar deadlock |
| Doble driver de OnGameTick | ✅ Mitigado | Dedup de 4ms por frame (el swap de buffers no es idempotente) |

---

## Offsets y RVAs citados en este archivo (verificados en código)

| Símbolo | Valor | Tipo | Fuente en código |
|---|---|---|---|
| `PlayerBase` (singleton GOG) | RVA `0x01AC8A90` | Función/global | core.cpp:785 |
| `GameWorld` (singleton GOG) | RVA `0x02133040` | Global (instancia embebida) | core.cpp:785 |
| `GameWorld.paused` | `+0x8B9` | Campo (byte) | core.cpp:2360 |
| `GameWorld.gameSpeed` | `+0x700` | Campo (float) | core.cpp:2360 |
| `GameWorld → player` | `+0x580` | Puntero | core.cpp:1891 |
| `player → participant (faction)` | `+0x2A0` | Puntero | core.cpp:1891 |
| `Character.faction` | `+0x10` | Puntero a Faction | core.cpp:2676, 2746, 2843 |
| `Faction.relation-read del motor` | `+0x250` | Campo (game+0x927E94 lo lee/tick) | core.cpp:2665, 2836 |
| `Faction.nameStr` | `+0x1A8` | std::string | core.cpp:1897 |
| `CharacterHuman.activePlatoon` | `+0x658` | Puntero | core.cpp:1186 |
| `platoon.activePlatoon` | `+0x1D8` | Puntero | core.cpp:1202 |
| `activePlatoon vtable → addMember` | `vtable+0x10` (slot 2) | Función virtual | core.cpp:1109 |
| RVA de crash de update de char | game+`0x927E94` | Sitio de lectura faction+0x250 | core.cpp:2665 |
| RVA de WRITE AV de squad injection | game+`0xE85340` | Sitio peligroso (desactivado) | core.cpp:2987 |

> ⚠ **Recordatorio de RE:** los **offsets de campo** (`+0x...`) son iguales Steam/GOG. Las **RVAs de
> función/global** (PlayerBase, GameWorld, game+0x927E94, game+0xE85340) cambian por versión/plataforma
> y deben re-verificarse con el Scanner, no asumirse.
