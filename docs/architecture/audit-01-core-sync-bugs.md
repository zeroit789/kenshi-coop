# Auditoría 01 — Bugs de Core + Sync (Kenshi Co-op)

> Auditoría **de solo lectura** del corazón del mod: `KenshiMP.Core/core.cpp`, el subsistema
> `sync/` y los orquestadores. Fecha: 2026-06-18. NO se ha modificado código fuente.
> Objetivo: race conditions, use-after-free, lecturas/escrituras sin SEH, gates que no se liberan,
> lógica incorrecta y cualquier cosa que afecte combate / simulación / estabilidad.
>
> Metodología: lectura directa del código + verificación cruzada de los modelos de hilos
> (render/Present vs lógica/TimeUpdate vs red vs 2 workers de fondo). Cada bug indica
> `archivo:línea`, severidad, descripción y FIX concreto. Se distingue **CONFIRMADO** de **SOSPECHA**.

---

## Modelo de hilos (recordatorio, verificado en código)

| Hilo | Entra por | Llama a |
|---|---|---|
| **Render / Present** (D3D11) | `render_hooks.cpp:449` | `OnGameTick()`, `PollForGameLoad()` (`:375`), `OnLoadingGapDetected()` (`:336/349`) |
| **Lógica / TimeUpdate** | `time_hooks.cpp:87` | `OnGameTick()` (redundante; "no dispara en Steam" pero SÍ en GOG/si empieza a disparar) |
| **Red** (`NetworkThreadFunc`) | `core.cpp:1856` | `m_client.Update()` → packet_handler → registry/interpolation/colas |
| **Workers de fondo (2)** | `TaskOrchestrator` | `BackgroundReadEntities` / `BackgroundInterpolate` (leen registry/escriben `m_frameData`) |

> El dato clave que habilita la mayoría de bugs CRÍTICOS: **`OnGameTick` lo conducen DOS hilos**
> (Present y TimeUpdate) y el único candado entre ellos es un dedup por tiempo de **4 ms con un
> `static` sin atomicidad**. No es exclusión mutua real.

---

## Tabla resumen (ordenada por impacto)

| # | Sev | Archivo:línea | Síntoma |
|---|-----|---------------|---------|
| 1 | **CRÍTICO** | core.cpp:1287-1294 (`OnLoadingGapDetected`) | Limpia registry/interp/frameData SIN `WaitForFrameWork` → UAF / heap corruption con los workers vivos |
| 2 | **CRÍTICO** | sync_orchestrator.cpp:328-353 (`Reset`) + core.h:144-167 | Disconnect/Reset sin drenar workers → UAF en `m_frameData`/registry. Mismo patrón que #1 |
| 3 | **CRÍTICO** | core.cpp:2269-2280 (dedup OnGameTick) | Dos hilos pueden ejecutar el pipeline a la vez; el guard de 4 ms no es exclusión mutua (data race en `s_lastTickTime`) |
| 4 | **CRÍTICO** | pending_snapshot_queue + packet_handler.cpp:644-649 | Flush de snapshots pendientes con timestamps desordenados / a entidad ya destruida |
| 5 | **ALTO** | sync_orchestrator.cpp:332/339 (`Reset`) | `ClearInterest(INVALID_PLAYER)` → el interés del jugador real no se limpia |
| 6 | **ALTO** | sync_orchestrator.cpp:494-571 vs 850-927 | Hilo de juego y worker tocan las MISMAS entidades locales → detección de movimiento no determinista |
| 7 | **ALTO** | entity_hooks.cpp:408 (`s_replayBackup`) | Buffer global único en `SEH_ReplayFactory`: se corrompe `reqStruct` si entra desde 2 hilos / reentrante |
| 8 | **ALTO** | pipeline_orchestrator.cpp:363-393 / sin purga de peers | Anomalías locales atadas al bucle de peers + peers desconectados nunca se purgan (falsos positivos perpetuos) |
| 9 | **MEDIO** | interpolation.cpp:147-169 (`GetInterpolated` const) | Snap-correction se decrementa N veces/frame (juego + 2 workers) → "pop" visual |
| 10 | **MEDIO** | sync_orchestrator.cpp:48-61 (`looksLikePointer`) | Heurística salta rotaciones de quaternion válidas → remotos miran mal |
| 11 | **MEDIO** | core.cpp:3810-3813 / sync_orchestrator.cpp:882 | Velocidad de fondo con `dist / 0.016f` fijo → animaciones a velocidad incorrecta |
| 12 | **MEDIO** | core.cpp:3338-3352 (FORCE BYPASS) | El bypass re-arranca el contador cada vez → re-espera 8 s en bucle |
| 13 | **MEDIO** | interpolation.cpp:175-178 + entity_registry (zona) | `Interpolation::m_entities` nunca se poda salvo `RemoveEntity` explícito → leak en sesiones largas |
| 14 | **MEDIO** | pending_snapshot_queue.cpp:49-81 | `CleanupOld` no purga entidad-fantasma que sigue recibiendo updates → leak lento |
| 15 | **BAJO** | core.cpp:3247-3252 (`SendCachedPackets`) | Código muerto: ya no se llama; mantiene `m_readBuffer`/`m_frameData` accesibles sin uso |
| 16 | **BAJO** | entity_registry.cpp (`m_nextId`) | `std::max(m_nextId, netId+1)` desborda si `netId == UINT32_MAX` → colisión con `INVALID_ENTITY` |

---

## CRÍTICOS

### BUG 1 — `OnLoadingGapDetected` limpia estado compartido sin drenar los workers de fondo
**`core.cpp:1287-1294`** · CONFIRMADO · UAF / corrupción de heap

```cpp
if (current == ClientPhase::GameReady) {     // load de partida en marcha
    m_entityRegistry.Clear();                // :1289
    m_interpolation.Clear();                 // :1290
    m_playerController.Reset();
    m_frameData[0].Clear();                  // :1292
    m_frameData[1].Clear();                  // :1293
    m_pipelineStarted = false;
```

Esto corre en el **hilo de render** (lo dispara `HookPresent` al detectar el gap de carga). En ese
momento los **workers de fondo** posteados en el último `OnGameTick` pueden estar dentro de
`BackgroundReadEntities`/`BackgroundInterpolate`, haciendo `writeFrame.localEntities.push_back(...)`
y leyendo punteros del registry (`m_registry.GetGameObject`). `FrameData::Clear()` hace
`vector::clear()` sobre el mismo vector → acceso concurrente sin lock → **corrupción de heap**.
Además `m_entityRegistry.Clear()` invalida los gameObjects que el worker está a punto de deref.

**FIX:** antes de cualquier `Clear()`, drenar el trabajo de fondo:
```cpp
if (m_pipelineStarted) m_orchestrator.WaitForFrameWork();
// y, si useSyncOrchestrator, también el TaskOrchestrator del sync (mismo m_orchestrator)
```
Colocarlo justo al entrar en el bloque `if (current == GameReady)`, ANTES de `m_entityRegistry.Clear()`.

---

### BUG 2 — `SyncOrchestrator::Reset()` y el disconnect handler no esperan a los workers
**`sync_orchestrator.cpp:328-353`** (`Reset`) + **`core.h:144-167`** (`SetConnected(false)`) · CONFIRMADO · UAF

`Reset()` hace `m_frameData[0].Clear()` / `m_frameData[1].Clear()` (`:336-337`) y `m_pipelineStarted=false`
**sin** `m_taskOrchestrator.WaitForFrameWork()`. El disconnect handler (`core.h`) hace ANTES
`m_entityRegistry.ClearRemoteEntities()` (`:145`), `m_interpolation.Clear()` (`:146`) y luego
`m_syncOrchestrator->Reset()` (`:166`) — todo sin drenar. Como cada tick postea 2 tareas y el
`WaitForFrameWork` solo ocurre al inicio del SIGUIENTE tick (`StageSwapBuffers`), al desconectar casi
siempre hay frame-work en vuelo. El worker hace `push_back` sobre el vector que el hilo que desconecta
está limpiando → corrupción. `SetConnected(false)` se invoca desde overlay/comandos/menú (hilo de
render/juego), pero los **workers siguen corriendo**, así que el contexto del llamador no protege.

**FIX:**
1. Al inicio de `Reset()` (antes de `:336`): `if (m_pipelineStarted) m_taskOrchestrator.WaitForFrameWork();`
2. En `core.h SetConnected(false)`: mover una llamada `m_orchestrator.WaitForFrameWork()` **antes** de
   `m_entityRegistry.ClearRemoteEntities()` (`:145`). (Verificar que el orquestador siga arrancado —
   lo está; `Stop()` solo en `Shutdown`.)

> BUG 1 y BUG 2 comparten raíz: **todo el contrato del doble buffer depende de `WaitForFrameWork`,
> pero los caminos de teardown (load in-game, disconnect) lo saltan.**

---

### BUG 3 — `OnGameTick` puede ejecutarse en paralelo desde dos hilos; el dedup de 4 ms no protege
**`core.cpp:2269-2280`** · CONFIRMADO (condicional a plataforma) · race / doble swap / corrupción

```cpp
static auto s_lastTickTime = std::chrono::steady_clock::time_point{};
auto now = std::chrono::steady_clock::now();
auto sinceLast = ... (now - s_lastTickTime);
if (sinceLast.count() < 4000) return;   // 4 ms
s_lastTickTime = now;
```

`OnGameTick` lo llaman Present (render) y TimeUpdate (lógica). El guard es un `static` leído y escrito
**sin atómica ni lock**. Si ambos hilos entran con >4 ms desde el último tick (arranque, hitch, carga),
los dos pasan el `if` y ejecutan el pipeline a la vez: doble `StageSwapBuffers` (revierte el swap),
doble drain de la command queue, doble `HandleSpawnQueue`. El propio comentario del código avisa que el
swap **no es idempotente**. En Steam hoy "TimeUpdate no dispara" (mitiga), pero el código de
`time_hooks.cpp:87` está activo y en GOG / si el motor empieza a llamar TimeUpdate, el bug es real.

**FIX:** convertir el tick en sección crítica real. Opción mínima: un `std::atomic<bool> s_tickInProgress`
con `compare_exchange_strong` al entrar y `store(false)` al salir (return temprano si ya hay un tick).
Opción robusta: un `std::mutex` con `try_lock` al inicio de `OnGameTick` (si no se obtiene, `return`).
Mantener además el dedup temporal para limitar frecuencia.

---

### BUG 4 — Flush de snapshots pendientes con orden temporal roto / a entidad muerta
**`pending_snapshot_queue.cpp:24-47`** + **`net/packet_handler.cpp:644-649`** · CONFIRMADO · corrupción lógica / entidad-zombi

Orden en packet_handler al recibir un spawn:
```
RegisterRemote(entityId, ...)
PendingSnapshotQueue::FlushForEntity(entityId)   // mete snapshots VIEJOS
float now = SessionTime();
AddSnapshot(entityId, now, spawnPos, rot)        // mete el más NUEVO después
```
Los snapshots flusheados llevan timestamps de cuando llegaron (antes del spawn); se insertan y JUSTO
DESPUÉS entra el de spawn con `now` mayor. El cálculo de velocidad en `AddSnapshot` deriva contra
`Get(0)` (último insertado), produciendo `dt` negativos/erráticos → velocidad basura → primer frame de
extrapolación catapulta al personaje. Además `FlushForEntity` no comprueba que la entidad siga viva: si
fue destruida entre `Queue` y `Flush`, `AddSnapshot` **recrea** la entrada en el interpolador (entidad
fantasma que nadie poda → ver BUG 13).

**FIX:**
1. Ordenar los pendientes por `timestamp` antes de flushear y descartar los que sean más viejos que el
   spawn (`if (snap.timestamp < now_spawn) continue;`).
2. En `FlushForEntity`, comprobar `registry.GetInfo(id)` válido y `state != Inactive` antes de inyectar;
   si no existe, `erase` y salir.

---

## ALTOS

### BUG 5 — `Reset()` limpia interés con `m_localPlayerId` ya invalidado
**`sync_orchestrator.cpp:332` y `:339`** · CONFIRMADO · fuga de estado entre sesiones

```cpp
m_localPlayerId = INVALID_PLAYER;      // :332
...
m_resolver.ClearInterest(m_localPlayerId);  // :339 ← ya es INVALID_PLAYER
```
Salvo que `ClearInterest(INVALID_PLAYER)` borre todo (no es así), el interés del jugador real no se
limpia → tras reconectar el resolver cree estar interesado en zonas de la sesión anterior.

**FIX:** capturar `PlayerID oldId = m_localPlayerId;` antes de `:332` y usar `ClearInterest(oldId)`.

---

### BUG 6 — Doble lectura de entidades locales (hilo de juego + worker) corrompe la detección de movimiento
**`sync_orchestrator.cpp:494-571`** (`StagePollAndSendPositions`) vs **`:850-927`** (`BackgroundReadEntities`) · CONFIRMADO

Ambos caminos leen las MISMAS entidades locales y llaman `UpdatePosition/UpdateRotation`, pisándose
`lastPosition`. El throttle por `KMP_POS_CHANGE_THRESHOLD` compara contra `lastPosition`: si el worker la
actualizó primero, el hilo de juego cree que no hubo movimiento y **no envía el update** → el remoto ve
al jugador "congelado" de forma intermitente. (El mismo patrón existe en la rama legacy: `PollLocalPositions`
en core.cpp:3162 + `BackgroundReadEntities` en core.cpp:3769.)

**FIX:** un único dueño de las entidades locales. Recomendado: que `BackgroundReadEntities` NO toque
entidades locales ni construya `packetBytes` (eliminar ese bloque), dejando al worker solo
remotas/interpolación. El envío por-entidad ya lo hace el hilo de juego.

---

### BUG 7 — `s_replayBackup`: buffer global único en `SEH_ReplayFactory` (slot MovRaxRsp compartido)
**`entity_hooks.cpp:408`** · CONFIRMADO (latente) · corrupción de `reqStruct`

```cpp
static uint8_t s_replayBackup[REQUEST_STRUCT_SIZE];   // :408 — UN solo buffer global
static void* SEH_ReplayFactory(...) {
    memcpy(s_replayBackup, reqStruct, structSize);   // save
    memcpy(reqStruct, preCallData, structSize);      // modify
    ... trampoline(...) ...                            // call (puede reentrar el hook)
    memcpy(reqStruct, s_replayBackup, structSize);   // restore
```
El patrón save→modify→call→restore con un **buffer global** solo es seguro si jamás se solapa. Hoy se
sostiene porque `CharacterCreate` viene de un único hilo y el guard `thread_local s_hookDepth`
(`entity_hooks.cpp:587`) evita reentrancia *dentro del hook*. Pero `SEH_ReplayFactory` llama al
trampolín que puede disparar otra creación; y si TimeUpdate empieza a conducir creates en paralelo a
otra ruta, `s_replayBackup` se clobbea entre el save de un thread y el restore de otro → `reqStruct`
del juego queda con datos de otra llamada → corrupción del struct de spawn. Es exactamente el riesgo de
"slots globales MovRaxRsp compartidos entre hilos".

**FIX:** mover `s_replayBackup` a buffer **local de la pila** dentro de `SEH_ReplayFactory`
(`uint8_t backup[REQUEST_STRUCT_SIZE];`). Es POD, válido dentro de `__try`. Elimina el estado global.
Igual revisar `s_preCallStruct`/`s_origCreate`/`s_directCallStub` y documentar que la cadena de creación
debe permanecer mono-hilo (atar a BUG 3: si se serializa OnGameTick, ayuda, pero la creación viene del
motor, no de OnGameTick).

---

### BUG 8 — PipelineOrchestrator: anomalías locales atadas al bucle de peers + peers nunca purgados
**`pipeline_orchestrator.cpp:363-393`** y ciclo de vida de `m_remoteSnapshots` · CONFIRMADO · falsos positivos perpetuos

Las comprobaciones locales (ghost mismatch sobre `m_localPlayerId`, CanSpawn, hook, phase) están DENTRO
del `for (peerId : m_remoteSnapshots)` → no se evalúan sin peers, y con N peers disparan
raise/resolve en flapping. Además no hay método para purgar un peer al desconectar: su snapshot queda
"congelado" → `SnapshotStale` y `SpawnQueueMismatch` falsos para siempre. `m_remoteSnapshotTimes[peerId]`
con `operator[]` (`:384`) puede insertar epoch → `elapsed` gigante → más falsos `SnapshotStale`.

**FIX:** sacar las comprobaciones locales fuera del bucle de peers; usar `find` en vez de `operator[]`
en `:384`; añadir `OnPeerDisconnect(PlayerID)` que borre de `m_remoteSnapshots`/`m_remoteSnapshotTimes`
bajo `m_remoteMutex`, llamado desde el handler de desconexión. (Este módulo es solo debug — no tumba el
juego — pero ensucia justo el HUD que se usa para diagnosticar "tracked:0" / "CharacterCreate:0".)

---

## MEDIOS

### BUG 9 — `GetInterpolated` const decrementa la snap-correction una vez por hilo
**`interpolation.cpp:147-169`** (y `.h:130-133`) · CONFIRMADO · "pop" visual

`GetInterpolated` es `const` pero muta `it->second.snap` vía `Apply(m_deltaTime)` (decrementa
`blendTimer`). La llaman el hilo de juego y los 2 workers para la MISMA entidad en el mismo frame → la
corrección de snap se consume hasta 3× más rápido. El comentario de `Update` asume una sola llamada por
frame, falso con workers.

**FIX:** mover el tick de la snap-correction a `Update()` (una vez/frame) y que `GetInterpolated` solo
LEA el offset sin decrementar. (Se simplifica mucho si se aplica BUG 6 y solo el worker de interpolación
llama a `GetInterpolated`.)

### BUG 10 — `looksLikePointer` salta rotaciones de quaternion válidas
**`sync_orchestrator.cpp:48-61`** · CONFIRMADO · remotos miran en dirección incorrecta

La guarda lee 8 bytes en `char+rotation` y, si "parecen" un puntero alineado, NO escribe la rotación. Dos
floats de un quaternion normalizado (~`0x3Fxxxxxx_3Fxxxxxx`) caen en `(0x10000, 0x7FFF...]` y alineados a
4 → falso positivo frecuente. El skip solo se loguea 5 veces → pasa desapercibido.

**FIX:** una vez el probe valide `offsets.rotation`, eliminar la heurística; mientras, exigir que
`existingVal` no parezca dos floats normalizados antes de clasificarlo como puntero.

### BUG 11 — Velocidad de fondo con `dt` fijo `0.016`
**`core.cpp:3810-3813`** y **`sync_orchestrator.cpp:882`** · CONFIRMADO · animaciones a velocidad errónea

`computedSpeed = dist / 0.016f` asume 62.5 FPS. El worker no corre a esa cadencia (depende del scheduler
y del throttle del tick) y Kenshi va a 30–60 FPS con multiplicador de tiempo. La velocidad sale mal
escalada → remotos corriendo cuando caminan. El cálculo del hilo de juego (`PollLocalPositions:3205-3207`)
sí usa el elapsed real.

**FIX:** pasar el `deltaTime`/elapsed real al worker, o (mejor, con BUG 6) que el worker no calcule
velocidad de entidades locales.

### BUG 12 — FORCE BYPASS de `HandleSpawnQueue` re-arranca el contador cada vez
**`core.cpp:3338-3352`** · CONFIRMADO · spawn re-bloqueado 8 s en bucle

Tras forzar el bypass a los 8 s, pone `s_gateTimerStarted = false` (`:3344`). La siguiente vez que el
gate vuelva a bloquear, el contador re-empieza desde 0 → otros 8 s de espera. Si el gate está
crónicamente bloqueado (p.ej. recursos Ogre nunca "listos"), los spawns pasan a tirones de 8 s en vez de
fluir.

**FIX:** una vez forzado el bypass para una sesión de carga, mantener un flag "ya forzado" hasta que
`pending==0`, en lugar de reiniciar el timer.

### BUG 13 — `Interpolation::m_entities` nunca se poda salvo `RemoveEntity` explícito
**`interpolation.cpp:175-178`** · CONFIRMADO · leak en sesiones largas

`AddSnapshot` crea entradas con `operator[]`. Si llega un snapshot para entidad ya destruida (BUG 4) o se
pierde el `RemoveEntity` (las descargas de zona del registry no llaman a `Interpolation::RemoveEntity`),
la entrada queda huérfana. Sin barrido por antigüedad, el mapa crece con NPCs transitorios.

**FIX:** poda por antigüedad del snapshot más reciente en `Update()`, y/o garantizar que toda ruta de
destrucción/zona llame `Interpolation::RemoveEntity`.

### BUG 14 — `PendingSnapshotQueue::CleanupOld` no purga entidad-fantasma que sigue recibiendo updates
**`pending_snapshot_queue.cpp:49-81`** · SOSPECHA · leak lento

`CleanupOld` borra snapshots >10 s, pero si el servidor sigue mandando posición de una entidad que el
cliente nunca spawnea, la clave se renueva indefinidamente con snapshots frescos.

**FIX:** límite duro por entidad (descartar si la cola de esa entidad supera N=32) y/o TTL por
clave-entidad además de por snapshot.

---

## BAJOS / hardening

### BUG 15 — `SendCachedPackets` es código muerto
**`core.cpp:3247-3252`** · CONFIRMADO · ruido / mantenimiento

El comentario en `OnGameTick` (`:2600-2604`) dice que se quitó porque duplicaba ancho de banda, pero la
función sigue definida y accede a `m_frameData[m_readBuffer.load()]`. No la llama nadie. Eliminarla
(y, si procede, el `SendCachedPackets()` del header) para no inducir a re-cablearla por error.

### BUG 16 — Posible overflow de `m_nextId` en el registry
**`entity_registry.cpp`** (`RegisterRemote`/`RemapEntityId`) · SOSPECHA · colisión con `INVALID_ENTITY`

`m_nextId = std::max(m_nextId, netId + 1)`: si un `netId` del servidor fuese `0xFFFFFFFF`, `netId+1`
desborda a 0 → un futuro `Register` devuelve 0 = `INVALID_ENTITY`.

**FIX:** `m_nextId = std::max(m_nextId, netId == UINT32_MAX ? netId : netId + 1);` o `uint64_t` para el
contador. (Baja probabilidad real, accionable trivial.)

---

## Lo que se REVISÓ y NO es bug (para no perder tiempo)

- **`EntityRegistry`**: todos los métodos públicos toman su lock (`shared_lock` lecturas / `unique_lock`
  escrituras). `GetInfo` devuelve **copia** (`std::optional<EntityInfo>`), no referencia → sin UAF de
  metadatos. `GetGameObject` devuelve `void*` por valor; el caller revalida bajo SEH (correcto).
- **`Interpolation`**: el ring buffer está bajo `m_mutex` en todas las rutas (`AddSnapshot`/`GetInterpolated`/
  `Update`). La validación NaN/Inf de posición y quaternion es correcta. (El único defecto es BUG 9, el
  decremento múltiple, no una race del buffer.)
- **`TaskOrchestrator::WaitForFrameWork`** (`task_orchestrator.cpp:83-88`): espera correctamente a
  `m_pendingFrameWork <= 0` con CV/mutex → es una barrera válida; el problema es que los caminos de
  teardown NO la usan (BUG 1, 2), no que esté mal implementada.
- **`s_hookDepth`** en `Hook_CharacterCreate` (`entity_hooks.cpp:587`): es `thread_local`, correcto.
- **`GameCommandQueue`** (`game_command_queue.h`): `DrainAll` hace swap bajo lock y ejecuta fuera del
  lock; correcto. (Detalle menor: `DrainAll` ignora el callback `fn` recibido y ejecuta `cmd.execute()`
  directamente — funciona porque el callback de OnGameTick hace lo mismo, pero es confuso; no es bug.)
- **Step 0 force-unpause** y **fix de facción del host**: la deref ciega de GameWorld embebido ya está
  arreglada con `GameWorldAccessor`. El fix de facción es one-shot/throttled/solo-host. Correctos.

---

## Recomendación de orden de arreglo

1. **BUG 1 + BUG 2 + BUG 3** juntos — son la misma familia (sincronización del doble buffer y del tick).
   Serializar `OnGameTick` (BUG 3) y drenar `WaitForFrameWork` en TODO teardown (BUG 1, 2) elimina la
   mayoría de los crashes de carga/desconexión.
2. **BUG 7** — quitar el buffer global `s_replayBackup` (cambio trivial, alto valor: protege el spawn).
3. **BUG 4** — orden de flush + validación de entidad viva (corrige catapultas de remotos al spawnear).
4. **BUG 5, 6** — limpieza de reconexión + dueño único de entidades locales (corrige "remoto congelado").
5. Resto (9-16) — calidad visual / leaks / hardening.

> Recordatorio RE: los `archivo:línea` de `core.cpp` son estables (es nuestro código). Las RVAs del juego
> citadas en los docs (PlayerBase, GameWorld, game+0x927E94) cambian por versión/plataforma y se resuelven
> por scanner — no afectan a estos bugs, que son de lógica/sincronización del propio mod.
