# 04 — Sistema de Sincronización (KenshiMP.Core/sync/)

> Mapa de arquitectura del subsistema de SINCRONIZACIÓN del mod Kenshi Co-op.
> Solo documentación. No modifica código. Fecha de mapeo: 2026-06-18.
> Rutas absolutas relevantes al final del documento.

---

## 0. Resumen ejecutivo (TL;DR)

El sistema de sync es un **pipeline de doble buffer (double-buffered)** que corre dentro
del tick del juego (`Core::OnGameTick`). Cada tick:

1. Lee la posición del jugador local desde su personaje (`GetPrimaryCharacter`).
2. Aplica posiciones interpoladas de personajes remotos a sus game objects.
3. Lee la posición de los personajes locales y la envía por red (no fiable, 20Hz).
4. Lee equipamiento local y envía cambios (fiable, 0.5Hz).
5. Procesa la cola de spawns (crea personajes remotos).
6. Lanza el trabajo de fondo (lectura de entidades + interpolación) para el siguiente frame.
7. Actualiza el estado de jugadores (AFK) + diagnósticos.

**Lo que SÍ se sincroniza:** posición, rotación (quaternion comprimido), velocidad de
movimiento, estado de animación, equipamiento (14 slots), salud por miembro (7 partes).

**Lo que NO se sincroniza (o está incompleto):** facción real (se PARCHEA localmente con la
facción del jugador host, no se replica), IA/órdenes de combate, reconciliación de predicción
local (`ReconcileLocal` es un TODO — solo cuenta, no aplica), navegación/pathfinding remoto,
estados de efectos (estructura existe pero sin ruta de red activa), inventario no equipado.

**Problemas conocidos** (sección 9): `tracked: 0`, `CharacterCreate: 0 calls`.

---

## 1. Arquitectura de componentes

```
                          Core::OnGameTick (game thread)
                                     │
              ┌──────────────────────┼─────────────────────────┐
              ▼                      ▼                           ▼
   Interpolation::Update    SyncOrchestrator::Tick      LoadingOrchestrator::Tick
   (avanza relojes)         (7 stages, ver §3)          (gating de spawn, ver §7)
                                     │
              ┌──────────────────────┼──────────────────────────┐
              ▼                      ▼                            ▼
       EntityRegistry         TaskOrchestrator              SpawnManager
       (estado de             (background workers)          (cola + factory)
        entidades)
```

### Clases núcleo (carpeta `sync/`)

| Componente | Archivo | Rol |
|---|---|---|
| **SyncOrchestrator** | `sync_orchestrator.{h,cpp}` | Orquestador principal. 7 stages por tick. Posee EntityResolver, ZoneEngine, PlayerEngine. Doble buffer `FrameData[2]`. |
| **Interpolation** | `interpolation.{h,cpp}` | Buffer de 8 snapshots por entidad (ring buffer). Jitter adaptativo, snap correction, dead-reckoning. |
| **EntityRegistry** | `entity_registry.{h,cpp}` | Mapa `netId → EntityInfo`. Fuente de verdad del estado replicado. `shared_mutex`. |
| **EntityResolver** | `entity_resolver.{h,cpp}` | Queries compuestas, spatial, interest management (3x3 zonas), dirty flags, ownership. |
| **ZoneEngine** | `zone_engine.{h,cpp}` | Tracking de zona local, índice zona→entidades, zona→jugadores. Rebuild cada 500ms. |
| **PlayerEngine** | `player_engine.{h,cpp}` | Máquina de estados de jugador (Connecting/Loading/InGame/AFK/Disconnected), sesiones, AFK. |
| **AuthorityValidator** | `authority_validator.{h,cpp}` | Decide qué hacer con cada snapshot entrante (aplicar/reconciliar/encolar/rechazar). |
| **SyncFacilitator** | `sync_facilitator.{h,cpp}` | Fachada/singleton de alto nivel para hooks/comandos. Delega a los engines. |
| **PipelineOrchestrator** | `pipeline_orchestrator.{h,cpp}` | Debugger de pipeline replicado por red (NO sincroniza juego). Snapshots 1Hz, anomalías, HUD. |
| **PipelineState** | `pipeline_state.h` | Estructuras del debugger: `PipelineSnapshot` (48 bytes), `PipelineEvent`, `PipelineAnomaly`. |
| **DeferredSpawnQueue** | `deferred_spawn_queue.{h,cpp}` | Spawns que llegaron antes de `GameReady`. Se procesan en `HandleAllPlayersReady`. |
| **PendingSnapshotQueue** | `pending_snapshot_queue.{h,cpp}` | Snapshots de posición que llegaron antes de que la entidad existiera. Flush al registrar. |
| **Ownership** | `ownership.cpp` | (helper de ownership) |
| **ZoneInterest** | `zone_interest.cpp` | (helper de interés por zona) |

### Componentes RELACIONADOS (fuera de `sync/`)

| Componente | Archivo | Relación con sync |
|---|---|---|
| **LoadingOrchestrator** | `game/loading_orchestrator.{h,cpp}` | Gating de spawn: máquina de fases de carga. |
| **SpawnManager** | `game/spawn_manager.{h,cpp}` | Cola de spawns + invocación de la factory de Kenshi. |
| **PlayerController** | `game/player_controller.{h,cpp}` | Personaje primario local, facción local, registro de remotos. |
| **entity_hooks** | `hooks/entity_hooks.{h,cpp}` | Hook de `CharacterCreate` → captura factory/facción + NPC hijack. **Origen del problema "CharacterCreate: 0 calls"**. |
| **char_tracker_hooks** | `hooks/char_tracker_hooks.{h,cpp}` | Hook inline de `CharAnimUpdate` → descubre personajes por nombre. **Origen del problema "tracked: 0"**. |
| **packet_handler** | `net/packet_handler.cpp` | Recepción de red → alimenta Interpolation + Registry. |
| **FrameData** | `sys/frame_data.h` | Contenedor double-buffer del pipeline. |
| **constants** | `KenshiMP.Common/include/kmp/constants.h` | Constantes de tick/interpolación. |

---

## 2. El tick de sincronización (orden global en Core)

`Core::OnGameTick` ejecuta el sync solo si `m_useSyncOrchestrator && m_syncOrchestrator`
(config `useSyncOrchestrator`). Orden real (core.cpp ~2503-2548), todo bajo wrappers SEH:

```
Step 2  interpolation     → SEH_InterpolationUpdate(deltaTime)   // avanza relojes de interp
Step 3  sync_orch_tick    → SEH_SyncOrchestratorTick(deltaTime)  // los 7 stages (§3)
Step 4  loading_orch      → SEH_LoadingOrchTick()                // gating de spawn (§7)
Step 5  handle_spawns     → HandleSpawnQueue()
        + ProcessDeferredAnimClassProbes()
        + DiagTickPump() / FixHostCharacterFactionTick (parche facción host)
        + RunOffsetProber (descubre offsets sceneNode/isPlayerControlled/aiPackage)
        + combat_hooks::ProcessDeferredEvents()
        + char_tracker_hooks::ProcessDeferredDiscovery()   // procesa ring de chars nuevos
        + world_hooks::ProcessDeferredZoneEvents()
```

Notas:
- **Todo el sync corre en el game thread** (hilo de lógica), NO en el render thread.
- El trabajo pesado de fondo (`BackgroundReadEntities`, `BackgroundInterpolate`) se delega
  al `TaskOrchestrator` y se recoge al inicio del tick siguiente (`StageSwapBuffers`).
- Cada stage crítico está envuelto en SEH para fallar con gracia (no tumbar el juego).

---

## 3. SyncOrchestrator: las 7 stages

`SyncOrchestrator::Tick(deltaTime)` (sync_orchestrator.cpp:359). Aborta si
`!m_active || m_localPlayerId == INVALID_PLAYER`. Incrementa `m_tickCount` al final.

### Stage 1 — `StageUpdateZones()`
- Lee posición del personaje primario (`SEH_ReadPosition`). Si es (0,0,0), aborta.
- `ZoneEngine::UpdateLocalPlayerZone(pos)` → si cambia de zona, actualiza `PlayerEngine`.
- Rebuild del índice de zonas cada `ZONE_REBUILD_INTERVAL_MS = 500ms` o al cambiar de zona.

### Stage 2 — `StageSwapBuffers()`
- Si `m_pipelineStarted`: `TaskOrchestrator::WaitForFrameWork()` (espera el background del
  frame anterior) y `std::swap(m_readBuffer, m_writeBuffer)`.
- Implementa el doble buffer: el background escribe en `writeBuffer`, el tick lee `readBuffer`.

### Stage 3 — `StageApplyRemotePositions()`
- Solo si `m_pipelineStarted` y `readFrame.ready`.
- Recorre `readFrame.remoteResults` (resultados interpolados del background):
  - Rechaza posiciones/rotaciones NaN/Inf (corromperían la memoria del motor → crash).
  - Valida el puntero del game object (rechaza SSO strings como `0x656E6F`="one", direcciones
    no-heap < `0x1000000`). Si es inválido, desvincula (`SetGameObject(nullptr)`).
  - `SEH_WritePositionRotation(gameObj, pos, rot)`: escribe posición + rotación con validación
    de quaternion (magnitud 0.5–1.5, no-NaN) y guarda contra escribir sobre lo que parezca un
    puntero (Ogre SceneNode*) en el offset de rotación.
  - `SEH_WriteMoveData(gameObj, moveSpeed, animState)`: escribe velocidad (decodifica u8→float
    0–15 m/s) y estado de animación, solo si los offsets fueron descubiertos por el prober.

### Stage 4 — `StagePollAndSendPositions()`
- Throttle a `KMP_TICK_INTERVAL_MS = 50ms` (20Hz).
- Para cada entidad local (`GetPlayerEntities(localId)`):
  - `SEH_ReadCharacterBG`: lee pos, rot, moveSpeed, animState vía CharacterAccessor.
  - Si el desplazamiento < `KMP_POS_CHANGE_THRESHOLD = 0.1`, salta (no envía).
  - Calcula velocidad (preferir lectura del juego; si no, derivar de delta de posición).
  - Comprime quaternion (`rot.Compress()` → u32). animState fallback: speed>5→2, >0.5→1.
  - Envía paquete `C2S_PositionUpdate` **no fiable** (1 entidad por paquete en main thread).
  - **Nota explícita en el código:** el background también construye un paquete cacheado en
    `packetBytes`, pero NO se envía aquí para evitar duplicar y doblar el ancho de banda.

### Stage 4b — `StagePollAndSendEquipment()`
- Throttle a `EQUIPMENT_POLL_INTERVAL_MS = 2000ms` (0.5Hz).
- Para cada entidad local, recorre `EquipSlot::Count` slots:
  - `SEH_ReadEquipmentSlot`: lee item del slot → template ID (offset `ItemOffsets::templateId=0x20`).
  - Diff contra `EntityInfo::lastEquipment[slot]`. Si cambió: actualiza cache + envía
    `C2S_EquipmentUpdate` **fiable**.

### Stage 5 — `StageProcessSpawns()` (el más complejo)
- **Heap scan con reintentos** (hasta 5, cooldown 5s): `ScanGameDataHeap()` + `FindModTemplates()`
  para localizar los templates de mod ("Player 1".."Player 16").
- **Timer de pendientes:** al aparecer el primer spawn pendiente, arranca contador y muestra
  mensajes HUD ("Waiting for game to create an NPC...", a 3s "Spawning...", a 15s aviso timeout).
- **Fallback de spawn directo:** si tras 15s el NPC hijack (Hook_CharacterCreate) no ha disparado
  y hay `HasPreCallData()`, intenta `SpawnCharacterDirect` (máx 5 intentos, 3s aparte):
  - Mapea owner→slot de template: `(owner-1) % modTemplateCount`.
  - `SEH_FixFactionAndSetup`: **parchea la facción** del char con la del jugador local (char+0x10),
    arreglando el use-after-free de facción (crash histórico en `faction+0x250`).
  - Setup post-spawn: `OnRemoteCharacterSpawned`, `MarkRemoteControlled`, `AddCharacterToLocalSquad`,
    `WritePlayerControlled(true)`, `ScheduleDeferredAnimClassProbe`.
  - Bug fixes incluidos: chequea que la entidad siga existiendo (owner pudo desconectar) y libera
    el spawn-cap al agotar reintentos (evita saturar el cap para siempre).

### Stage 6 — `StageKickBackgroundWork()`
- Limpia el `writeBuffer`, postea al `TaskOrchestrator` dos tareas de fondo:
  - `BackgroundReadEntities()`: lee entidades locales, construye `localEntities` + `packetBytes`.
  - `BackgroundInterpolate()`: para cada entidad remota, llama a `Interpolation::GetInterpolated`
    y rellena `remoteResults` (consumido por Stage 3 del próximo tick).
- Marca `m_pipelineStarted = true`.

### Stage 7 — `StageUpdatePlayers(deltaTime)`
- AFK check cada 200 ticks (~10s): `PlayerEngine::CheckAFK()`.
- Diagnósticos cada 5s: ticks/s, conteo de entidades, remotas, zona local.

---

## 4. Sistema de interpolación (8 snapshots)

`Interpolation` (interpolation.{h,cpp}). Buffer **ring de 8 snapshots por entidad**
(`KMP_MAX_SNAPSHOTS = 8`), protegido por `mutex`. Cada `EntityInterpState` tiene:
- `Snapshot buffer[8]` (timestamp, posición, velocidad calculada, rotación, moveSpeed, animState).
- `JitterEstimator`: EMA de la varianza inter-paquete → delay adaptativo.
- `SnapCorrection`: blend de corrección de error de posición.

### Flujo de entrada — `AddSnapshot()`
- Rechaza NaN/Inf en posición y quaternion.
- Calcula velocidad desde el snapshot previo (si `0.001 < dt < 2.0`).
- Detección de discontinuidad (snap correction):
  - error = pos_real − (pos_prev + vel_prev·dt).
  - `> KMP_SNAP_THRESHOLD_MAX (50)` → teletransporte (sin blend, salta).
  - `> KMP_SNAP_THRESHOLD_MIN (5)` → arranca blend de corrección (`KMP_SNAP_CORRECT_SEC = 0.15s`).
  - `< 5` → la interpolación normal lo absorbe.

### Flujo de salida — `GetInterpolated(entityId, renderTime, ...)`
- `interpTime = renderTime − jitter.GetDelay()` (delay adaptativo 50–200ms,
  default `KMP_INTERP_DELAY_SEC = 0.1`).
- Busca snapshots "before/after" que rodeen `interpTime`:
  - **Caso 1:** sin datos → false.
  - **Caso 2:** solo futuros → usa el más temprano.
  - **Caso 3:** todo en el pasado → **extrapolación / dead-reckoning**: `pos + vel·overshoot`,
    capado a `KMP_EXTRAP_MAX_SEC = 0.25s`.
  - **Caso 4:** interpolación normal: lerp lineal de posición + `Quat::Slerp` de rotación.
    Estados discretos (moveSpeed/animState) usan el snapshot más cercano.
- Aplica `SnapCorrection` con decay en tiempo real.

### Jitter adaptativo (`JitterEstimator`)
- `KMP_JITTER_EMA_ALPHA = 0.1`. Mapea jitter→delay: 20ms jitter→50ms delay, 80ms→200ms.

> Nota de timing: `Interpolation::Update` solo avanza relojes; los timers de snap correction
> se decrementan dentro de `SnapCorrection::Apply` (llamado desde `GetInterpolated`), para
> evitar doble decremento.

### Constantes (`kmp/constants.h`)
```cpp
KMP_TICK_RATE            = 20            // 20 Hz state sync
KMP_TICK_INTERVAL_MS     = 50           // 1000/20
KMP_TICK_INTERVAL_SEC    = 0.05
KMP_INTERP_DELAY_SEC     = 0.1          // 100ms buffer default
KMP_MAX_SNAPSHOTS        = 8            // ring buffer por entidad
KMP_INTERP_DELAY_MIN     = 0.05         // 50ms delay adaptativo min
KMP_INTERP_DELAY_MAX     = 0.2          // 200ms delay adaptativo max
KMP_EXTRAP_MAX_SEC       = 0.25         // 250ms max extrapolación
KMP_SNAP_THRESHOLD_MIN   = 5.0          // blend suave por debajo
KMP_SNAP_THRESHOLD_MAX   = 50.0         // teletransporte por encima
KMP_SNAP_CORRECT_SEC     = 0.15         // duración blend de corrección
KMP_JITTER_EMA_ALPHA     = 0.1
KMP_POS_CHANGE_THRESHOLD = 0.1          // movimiento mínimo para enviar update
```

---

## 5. Spawn pipeline (cómo se crean personajes remotos)

Hay **tres rutas** para materializar un personaje remoto. Todas terminan llamando a la
factory interna de Kenshi (RootObjectFactory::create) y luego parchean la facción.

### Ruta A — NPC Hijack (preferida, vía `Hook_CharacterCreate`)
Cuando el juego crea un NPC propio (caminar cerca de un pueblo/campamento dispara creates),
el hook en modo conectado (entity_hooks.cpp ~918) hace:
1. `SpawnManager::PopNextSpawn()` saca una `SpawnRequest` de la cola.
2. Chequea el cap por jugador (`MAX_SPAWNS_PER_PLAYER`).
3. `SEH_HijackNPC(character, netId, owner, pos)`: **toma posesión del NPC recién creado**
   (ya tiene facción/modelo/animaciones válidos → cero riesgo de crash de facción).
   Lo registra, teletransporta a la posición remota, renombra y desactiva su IA.
4. Cuenta como `inPlaceSpawn`.

### Ruta B — Spawn directo (fallback, vía `SyncOrchestrator::StageProcessSpawns`)
Si el hijack no dispara en 15s → `SpawnCharacterDirect` (clon de struct pre-call) +
`SEH_FixFactionAndSetup`. Descrito en §3 Stage 5.

### Ruta C — Spawn diferido (`DeferredSpawnQueue` / `ProcessDeferredSpawn`)
Spawns que llegaron por red antes de `ClientPhase::GameReady`. Se encolan y procesan en
`HandleAllPlayersReady`. `ProcessDeferredSpawn` (packet_handler.cpp:1865) hace
`RegisterRemote` + `FlushForEntity` (vuelca snapshots pendientes) + `AddSnapshot` inicial.

### Recepción de red (packet_handler.cpp)
- **Spawn entrante** → `EntityRegistry::RegisterRemote(netId, type, owner, pos)` (estado
  `Spawning`), luego `PendingSnapshotQueue::FlushForEntity` y `AddSnapshot` inicial.
- La entidad pasa de `Spawning → Active` cuando `SetGameObject` vincula un game object real.

### EntityInfo (estado replicado por entidad)
`entity_registry.h:10`. Campos clave:
```cpp
struct EntityInfo {
    EntityID      netId;                  // ID de red
    uint32_t      generation;             // anti-ghost (rechaza paquetes viejos)
    void*         gameObject;             // puntero al char del juego (null hasta spawn)
    EntityType    type;
    PlayerID      ownerPlayerId;          // 0 = server-owned
    EntityState   state;                  // Inactive/Spawning/Active
    AuthorityType authority;
    LocalAuthorityState localState;       // LocalOwned/RemoteOwned/ServerOwned
    Vec3          lastPosition, velocity;
    Quat          lastRotation;
    float         health;
    LimbHealth    limbs;                  // 7 partes
    uint8_t       statusEffects[5];       // estructura presente, sin ruta de red activa
    uint8_t       moveSpeed, animState;
    bool          isRemote;
    uint32_t      lastEquipment[14];      // diff de equipamiento por slot
};
```
`m_nextId = 0x10000000` (IDs locales arrancan alto para no colisionar con los del servidor).

---

## 6. Tracking de entidades

### EntityRegistry
- Mapa `netId → EntityInfo` + mapa inverso `void* gameObject → netId`. `shared_mutex`.
- Validación de punteros en `Register`/`SetGameObject`: rechaza < `0x10000`, > userspace,
  o no alineados a 4 (`addr & 0x3`).
- Transición automática `Spawning → Active` al vincular game object.
- Queries: `GetPlayerEntities(owner)`, `GetRemoteEntities()`, `GetEntitiesInZone`, conteos.
- Helpers de autoridad: `IsLocalOwned`, `IsRemoteOwned`, `IsServerOwned`, `IsValidGeneration`.

### EntityResolver (interest management)
- `ComputeInterest(playerId, zone)`: calcula entidades en grid 3x3 de zonas + deltas
  enter/leave. Cache por jugador (máx 16).
- Dirty flags: `MarkDirty`/`ConsumeDirty` para replicación selectiva.

### char_tracker_hooks (descubrimiento por nombre)
- Hook **inline** sobre `CharAnimUpdate` (dispara ~300 veces/seg). El cuerpo del hook NO
  hace heap alloc / mutex / spdlog: solo empuja `(animClassPtr, charPtr)` a un **ring buffer
  lock-free de 128**. `charPtr` se lee de `animClass+0x2D8`.
- `ProcessDeferredDiscovery()` (desde OnGameTick, máx 8/tick) hace el trabajo caro: lee nombre
  vía CharacterAccessor, construye `TrackedChar`, inserta en `s_trackedChars`.
- API: `FindByName`, `FindByPtr`, `GetLocalPlayerAnimClass`, `GetTrackedCount`.

---

## 7. Gating de carga / burst (LoadingOrchestrator)

Máquina de fases que **bloquea spawns durante la carga** para evitar corrupción de heap
y crashes. Fases (`LoadingPhase`): `Idle / InitialLoad / ZoneTransition / SpawnLoad`.
Arranca en `InitialLoad`.

### `IsSafeToSpawn()` exige TODO:
1. Fase == `Idle`.
2. NO en burst (`m_inBurst == false`).
3. `m_gameLoaded == true` (lo pone `OnGameLoaded`).
4. Sin recursos pendientes (solo si hay hooks de recurso Ogre instalados).
5. Cooldown `SPAWN_COOLDOWN_MS = 2000ms` desde la última carga de recurso.

`GetSpawnBlockReason()` devuelve la razón humana del bloqueo (útil para el HUD `/pipeline`).

### Detección de burst (integrada con entity_hooks)
- `OnBurstDetected(count)`: marca `m_inBurst`, y si estaba `Idle` con juego cargado, pasa a
  `ZoneTransition` (un burst durante gameplay = carga de zona).
- `OnBurstEnded()`: limpia burst, vuelve a `Idle` desde InitialLoad/ZoneTransition.
- **Timeout de seguridad** `BURST_TIMEOUT_MS = 30000ms`: si `OnBurstEnded` nunca llega
  (evento perdido), auto-limpia el burst para no quedar bloqueado para siempre.
- `ZONE_SETTLE_MS = 1500ms`: espera tras carga de zona antes de volver a `Idle`.

### Modos del hook de CharacterCreate (entity_hooks.cpp)
- **LOADING PASSTHROUGH** (`s_loadingPassthrough = true`): instalado así al arrancar. Durante
  la carga del save (~130+ creates), el hook SOLO actualiza timestamp/contador, captura el
  puntero de factory y nombres de templates ("Player N"), y llama al original. Sin lecturas de
  memoria pesadas. Evita el stack-swap MovRaxRsp en ráfaga (que corrompe el heap).
- **CONNECTED / FULL** (tras `ResumeForNetwork`): cuerpo completo — registro, NPC hijack,
  bootstrap de facción (voting multi-fuente), detección de offsets (posición, GameData).
- **DIRECT SPAWN BYPASS** (`s_directSpawnBypass`): cuando SpawnManager llama a la factory él
  mismo, el hook pasa directo al original (evita recursión).
- `SuspendForDisconnect()`: vuelve a passthrough al desconectar (bursts de descarga de zona
  no disparan la lógica completa).

---

## 8. Autoridad y recepción de snapshots

`AuthorityValidator::ValidateInboundSnapshot` (authority_validator.cpp) decide por cada
`CharacterPosition` entrante en `HandlePositionUpdate`:

| Condición | Decisión |
|---|---|
| Entidad no existe en registry | `QueuePendingSpawn` → `PendingSnapshotQueue::Queue` |
| `info.generation != pos.generation` | `RejectStaleGeneration` (anti-ghost, **Phase 6 activo**) |
| `state == Inactive` | `RejectDestroyed` |
| `ownerPlayerId == myPlayerId` (mi entidad de vuelta del servidor) | `ReconcileLocal` |
| `ownerPlayerId != sourcePlayerId` (violación de autoridad) | `RejectAuthorityViolation` |
| Remota con owner válido | `ApplyRemote` → `Interpolation::AddSnapshot` |

> **`ReconcileLocal` está SIN IMPLEMENTAR** (TODO Phase 7): solo cuenta `reconciledLocal++`
> y descarta para no provocar rubber-banding. No hay reconciliación de predicción cliente.

### Qué se aplica a memoria del juego al recibir
- **Posición/rotación:** vía interpolación → Stage 3 (`SEH_WritePositionRotation`).
- **Salud:** `HandleHealthUpdate` escribe por cadena de punteros
  `char + healthChain1 → +healthChain2 → +healthBase + i·healthStride` para 7 partes (SEH).
  También `UpdateLimbHealth` en el registry.
- **Equipamiento:** `HandleEquipmentUpdate` (slot → template ID).
- **MoveCommand:** no llama a la función MoveTo del juego (es mid-function, insegura) — en su
  lugar inyecta un snapshot futuro (`now + 1.0`) en la interpolación.

---

## 9. Problemas conocidos y diagnóstico

### "tracked: 0" (char_tracker)
`GetTrackedCount()` devuelve 0 cuando `s_trackedChars` está vacío. Causas posibles:
- **El hook inline de `CharAnimUpdate` no se instaló:** `funcs.CharAnimUpdate` sin resolver
  (RVA no encontrada por el scanner) → `Install()` retorna false con warning. Sin hook, nunca
  se empujan chars al ring buffer.
- **`ProcessDeferredDiscovery` no corre o se queda sin procesar:** solo procesa si hay juego
  cargado y se llama desde OnGameTick. Si el ring se llena más rápido de lo que se drena
  (8/tick), se descartan updates.
- **Nombres vacíos:** `accessor.GetName()` vacío → el char se descarta (no se inserta).
- **Verificación sugerida:** comprobar log `char_tracker: Inline hook installed at 0x...`;
  si falta, el problema es la resolución de `CharAnimUpdate` en el scanner (RVA por versión).

### "CharacterCreate: 0 calls" (entity_hooks)
`GetTotalCreates()` (= `s_totalCreates`) en 0 significa que `Hook_CharacterCreate` nunca
disparó. Pero OJO: el contador `s_totalCreates` solo incrementa **fuera** del direct-spawn
bypass. Causas posibles:
- **El hook no se instaló:** `funcs.CharacterSpawn` (RVA de la factory) sin resolver →
  `InstallAt("CharacterCreate", ...)` no se ejecuta. Verificar log
  `entity_hooks: hooking CharacterCreate at 0x...`.
- **El juego no está creando NPCs:** el hijack y el contador dependen de que el motor cree
  personajes. Si el jugador está lejos de pueblos/campamentos, no hay creates → la cola de
  spawns se queda pendiente y aparecen los mensajes "walk near a town" (Stage 5).
- **Hook suspendido:** tras `SuspendForDisconnect`, o si `HookManager::Enable("CharacterCreate")`
  falló en `ResumeForNetwork` (warning "Enable() returned false") → el hook está físicamente
  desinstalado/deshabilitado y no incrementa nada.
- **MovRaxRsp:** la factory de Kenshi usa prólogo `mov rax, rsp`. El wrapper de hook debe
  preservar RSP correctamente; el bypass y el raw trampoline (`s_directCallStub` con
  `mov rax,rsp; jmp tramp+3`) existen precisamente para esto. Un fallo aquí se manifiesta
  como crash, no como "0 calls" — pero confirma que la cadena de instalación es frágil por versión.
- **Relación con "tracked:0":** ambos síntomas juntos apuntan casi siempre a **RVAs no
  resueltas por el scanner** (CharacterSpawn / CharAnimUpdate). Verificar `patterns.json` y
  los logs de resolución de funciones antes que la lógica de sync.

### Otros estados desactivados / incompletos
- **`statusEffects[5]`**: estructura presente en EntityInfo pero sin ruta de red activa.
- **Facción**: NO se replica realmente; se **parchea localmente** con la facción del host
  (char+0x10) para evitar el use-after-free. Es la causa raíz documentada del pendiente
  "enemigos huyen / facciones" (los remotos no tienen la facción correcta).
- **CharacterDestroy hook**: NO instalado (`s_destroyHookInstalled = false`). El conteo de
  spawn-cap se decrementa manualmente vía `DecrementSpawnCount`.
- **Hooks de recurso Ogre**: `m_hasResourceHooks` casi siempre false → el gating no espera
  recursos reales, solo usa burst + cooldown.

### PipelineOrchestrator (debug, NO sincroniza juego)
Sistema de telemetría replicado por red para depurar el propio pipeline: snapshots de 48 bytes
a 1Hz entre peers, detección de anomalías (`CanSpawnStuck`, `HookNotFiring`, `PhaseStuck`,
`SnapshotStale`...), HUD toggleable y comando `/pipeline`. Útil para diagnosticar
exactamente "tracked:0" y "CharacterCreate:0" en runtime entre jugadores.

---

## 10. Impacto en la simulación local de personajes (resumen)

| Aspecto | Estado |
|---|---|
| Posición/rotación remota | Aplicada cada frame (interpolada, con validación NaN/Inf + guard de puntero). |
| Velocidad/animación remota | Aplicada si el prober descubrió los offsets; si no, no-op seguro. |
| Salud remota | Escrita por cadena de punteros (SEH). |
| Equipamiento remoto | Escrito por slot. |
| **Facción** | **Sobrescrita localmente** con la del host → remotos no tienen su facción real. |
| **IA remota** | Desactivada (`MarkRemoteControlled`, `WritePlayerControlled`). No se simula IA del remoto. |
| **Predicción/reconciliación local** | NO implementada (ReconcileLocal = TODO). |
| **Pathfinding/órdenes** | No replicado (MoveCommand se simula vía snapshot futuro de interp). |
| Anti-ghost (generación) | Activo (Phase 6). |
| Validación de autoridad | Activa (rechaza violaciones de owner/generación/destruido). |

---

## Archivos relevantes (rutas absolutas)

Sync core:
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\sync_orchestrator.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\interpolation.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\entity_registry.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\entity_resolver.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\zone_engine.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\player_engine.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\authority_validator.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\sync_facilitator.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\pipeline_orchestrator.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\pipeline_state.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\deferred_spawn_queue.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sync\pending_snapshot_queue.cpp` / `.h`

Relacionados:
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\game\loading_orchestrator.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\game\spawn_manager.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\game\player_controller.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\hooks\entity_hooks.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\hooks\char_tracker_hooks.cpp` / `.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\net\packet_handler.cpp`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\sys\frame_data.h`
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Core\core.cpp` (OnGameTick ~2503)
- `E:\Aplicaciones\Kenshi-Online\KenshiMP.Common\include\kmp\constants.h`
