# Auditoría 11 — Sincronización del 2º jugador: plan para estabilizar el co-op

> Investigación **SOLO LECTURA**. No se modificó código. Fecha: 2026-06-19.
> Binario: `kenshi_x64.exe` v1.0.68 Steam (App 233860), MSVC x64, ImageBase `0x140000000`.
> Objetivo: tras el fix de combate de 1 jugador (host), preparar el **co-op real (2 jugadores)**.
> El muro conocido (Estado del Arte): nadie ha logrado combate sincronizado en Kenshi (motor mono-hilo).
> Cruzado con: `audit-10` (revisión general, BUG 17/18/19/C-1/C-2/C-3), `audit-01` (threading 1-16),
> `audit-03` (combate host), código real de `KenshiMP.Core/{net,game,sync,hooks}` y notas de RE.
>
> **Hallazgo central confirmado en código:** todos los packet handlers
> (`HandleEntitySpawn`, `HandleAllPlayersReady`→`ProcessDeferredSpawn`, `HandleHealthUpdate`,
> `HandleStatUpdate`, `HandleCombatKO`, etc.) corren en el **hilo de RED**
> (`Core::NetworkThreadFunc` @ core.cpp:1920 → `m_client.Update()` → `enet_host_service` →
> `SetPacketCallback` → `HandlePacket` @ packet_handler.cpp:138). El init del char remoto escribe
> al motor desde ese hilo sin marshalar al hilo de juego. Ese es el crash del 2º jugador.

---

## 0. TL;DR — qué pasa y qué hacer

Cuando el 2º jugador conecta, el host recibe `S2C_EntitySpawn` (o `S2C_AllPlayersReady` que dispara
los spawns diferidos) **en el hilo de red**. El código de "link existing mod character"
(`packet_handler.cpp:669-744`) ejecuta TODO el init del char remoto ahí mismo: `WriteName`,
`WriteNameToGameData`, sonda de anim, `WriteLimbHealth`, `MarkRemoteControlled` y `SEH_AllyModFaction`.
Solo la **posición** se difiere correctamente al `CommandQueue`. El resto corrompe estructuras del
motor (Ogre/anim/GameData/faction) que el hilo de juego está tocando a la vez → crash.

El patrón correcto **ya existe** en dos formas en el repo:
- `GameCommandQueue` (`game_command_queue.h`): marshala lambdas de **hilo de red → hilo de juego**.
  Se drena en `Core::OnGameTick` @ core.cpp:4985, ANTES del pipeline de sync. Usado bien en 4 sitios.
- El **ring SPSC lock-free** de `combat_hooks.cpp`: marshala de **hook (game thread) → game tick**
  (mismo hilo, productor=consumidor). NO sirve para red→juego (ese es el del CommandQueue).

**El trabajo es migrar TODO el camino de spawn remoto + los ~9 handlers que tocan el motor al
`GameCommandQueue`.** No hay que inventar nada nuevo: hay que aplicar un patrón que el propio mod
ya usa correctamente para CombatHit/CombatDeath.

**Orden recomendado:**
- **F0** — Estabilizar el 2º jugador: migrar `HandleEntitySpawn` (link) y `ProcessDeferredSpawn` al
  CommandQueue, neutralizar `SEH_AllyModFaction` (BUG 18), serializar la votación de facción (C-2),
  resetear el `SpawnManager` en disconnect (A-3), y blindar `s_savedChecks` (C-3).
- **F1** — Migrar los handlers de estado (Health/Limb/Stat/Equipment/Inventory/KO/Door/FactionRelation)
  al CommandQueue (BUG 19).
- **F2** — Drenado seguro del CommandQueue en teardown (atado a audit-01 BUG 1/2: `WaitForFrameWork`
  + `CommandQueue.Clear()` en disconnect).
- **F3** — Combate autoritativo entre jugadores (replicar daño/KO/muerte): qué es realista y dónde
  está el muro del motor mono-hilo.

---

## 1. Flujo de spawn de un char REMOTO (cuando entra el 2º jugador)

### 1.1 Las dos rutas de spawn

**Ruta A — "link existing mod character" (la principal y la que crashea).**
`HandleEntitySpawn` (packet_handler.cpp ~600-777). El `kenshi-online.mod` precrea "Player 1..16" al
cargar la partida. El handler busca el char por slot (`FindModCharacterBySlot`) y lo enlaza a la
entidad de red. Bloque crítico **packet_handler.cpp:669-744**:

| Línea | Acción | Hilo actual | ¿Difiere? |
|---|---|---|---|
| 671 | `registry.SetGameObject` + `UpdatePosition` | red | n/a (registry tiene su lock) |
| 675 | `ai_hooks::MarkRemoteControlled(existingChar)` | red | **NO** (toca estado del char) |
| 696-703 | `SEH_WritePositionToChar` | red→**juego** | **SÍ** (CommandQueue) ✅ |
| 708 | `SEH_OnRemoteCharSpawned` → `accessor.WriteName()` (player_controller.cpp:152) | red | **NO** ❌ |
| 712 | `WriteGameDataNameForModLink` → `WriteNameToGameData` | red | **NO** ❌ |
| 715 | `SEH_ScheduleAnimProbe` (pipeline de animación Ogre) | red | **NO** ❌ |
| 720-724 | `UpdateLimbHealth` + `SEH_WriteLimbHealthToChar` (cadena +0x2B8→+0x5F8→+0x40) | red | **NO** ❌ |
| 736 | `SEH_AllyModFaction` (BUG 18, faction-UAF) | red | **NO** ❌ |

El SEH evita que un AV mate el hilo de red, pero **no impide la corrupción**: si el hilo de juego
está leyendo el GameData/anim/faction del mismo char mientras la red lo reescribe, el dato queda
inconsistente sin que salte ninguna excepción.

**Ruta B — fallback factory (`SpawnManager::QueueSpawn`).**
Si no hay char de mod, se encola una `SpawnRequest` (packet_handler.cpp:750-776). Esta ruta SÍ es
segura: `QueueSpawn` solo mete en una cola con mutex (spawn_manager.cpp:276); el spawn real lo hace
`ProcessSpawnQueueFromHook` desde **dentro de `Hook_CharacterCreate`** (game thread, contexto
correcto). El spawn por replay-in-place está bien diseñado. **No tocar esta ruta** salvo el reset de
estado (A-3).

**Ruta C — spawn diferido (`ProcessDeferredSpawn`, packet_handler.cpp:1865-1939).**
La llama `DeferredSpawnQueue::ProcessAll()` desde **`HandleAllPlayersReady` (línea 134)** — que es un
packet handler → **hilo de red**. Replica el mismo bug de la Ruta A: difiere posición (1905) pero
ejecuta `SEH_OnRemoteCharSpawned`, `WriteGameDataNameForModLink`, `SEH_ScheduleAnimProbe`,
`SEH_WriteLimbHealthToChar` inline. **Mismo fix que la Ruta A.**

### 1.2 Cómo migrarlo al CommandQueue

El patrón objetivo (idéntico al de packet_handler.cpp:696-703, ya correcto): capturar por **valor**
todo lo necesario y meter el init completo en un único `core.GetCommandQueue().Push({ ... })`.

Reglas de diseño del marshalado (importantes para no reintroducir bugs):

1. **Un solo Push por char**, no uno por sub-paso. El init de un char remoto debe ejecutarse atómico
   respecto al tick (todo o nada en el mismo drenado), no esparcido en varios comandos que se mezclen
   con otros.
2. **Capturar por valor**: `existingChar` (void*), `entityId`, `ownerId`, `bestPos/rot`, `healthData[7]`,
   `hasExtended`. NUNCA capturar por referencia (los locales del handler de red ya no existen cuando el
   tick drena la cola). Esto ya lo advierte el comentario en :957 ("Capture by VALUE only").
3. **Revalidar dentro del lambda**: `core.IsGameLoaded() && GetClientPhase() >= GameReady` (como ya hace
   el Push de posición). Y revalidar que `existingChar` sigue registrado (que `registry.GetNetId` ==
   entityId), por si la entidad se despawneó entre el encolado y el drenado.
4. **Lo que NO necesita CommandQueue** (puede quedarse en hilo de red): los updates a `registry.*`
   (tienen lock propio), `AddSnapshot`/interpolación (mutex propio), `RegisterRemote`,
   `PendingSnapshotQueue` y `VisualProxy` (estado del mod, no del motor). Solo lo que TOCA MEMORIA DEL
   MOTOR o LLAMA FUNCIONES NATIVAS va a la cola.

Esquema del bloque migrado (sustituye 669-744 y el equivalente en ProcessDeferredSpawn):

```cpp
if (existingChar) {
    // --- En hilo de red: solo metadatos del mod (con sus propios locks) ---
    registry.SetGameObject(entityId, existingChar);
    registry.UpdatePosition(entityId, spawnPos);
    if (hasExtended) registry.UpdateLimbHealth(entityId, healthData);
    // VisualProxy / RemotePlayer tracking aquí también (no tocan el motor).

    // --- Marshalar TODO el init del motor a un solo comando del game thread ---
    float healthCopy[7]; for (int i=0;i<7;i++) healthCopy[i]=healthData[i];
    core.GetCommandQueue().Push({[existingChar, entityId, ownerId, spawnPos, rot,
                                  hasExtended, healthCopy]() {
        auto& core = Core::Get();
        if (!core.IsGameLoaded() || core.GetClientPhase() < ClientPhase::GameReady) return;
        // Revalidar que el char sigue ligado a esta entidad
        if (core.GetEntityRegistry().GetNetId(existingChar) != entityId) return;

        ai_hooks::MarkRemoteControlled(existingChar);                 // mov 675
        SEH_WritePositionToChar(existingChar, spawnPos.x, spawnPos.y, spawnPos.z);
        SEH_OnRemoteCharSpawned(entityId, existingChar, ownerId);     // WriteName
        core.GetPlayerController().WriteGameDataNameForModLink(existingChar, ownerId);
        SEH_ScheduleAnimProbe(existingChar);
        if (hasExtended) SEH_WriteLimbHealthToChar(existingChar, healthCopy);
        // SEH_AllyModFaction(existingChar);  // <-- NO; ver §3 (BUG 18). Sustituir.
    }});
}
```

> Nota: `MarkRemoteControlled`/`UnmarkRemoteControlled` usan un mutex propio en `ai_hooks`
> (audit-10 lo marca como "bien protegido"), así que estrictamente podría quedarse en hilo de red;
> meterlo en el comando es más limpio (queda junto al resto del init) y no daña.

---

## 2. El CommandQueue del mod — cómo funciona y cómo marshalar escrituras

### 2.1 Mecánica (game_command_queue.h, confirmado)

- `GameCommand` = wrapper de `std::function<void()>`.
- `Push(cmd)` — thread-safe (mutex), lo llama cualquier hilo (red).
- `DrainAll(fn)` — en el game thread: hace `swap` del vector bajo lock y ejecuta **fuera** del lock
  (correcto: no se puede sostener el lock mientras corre código del motor que podría re-encolar).
- `Clear()` — vacía sin ejecutar (para disconnect).
- **Drenado**: `Core::OnGameTick` @ core.cpp:4985-5000, **antes** del pipeline de sync (interpolación,
  sync orchestrator, HandleSpawnQueue). Buen sitio: el estado del motor ya está estable al inicio del
  tick.

### 2.2 Qué marshalar (la regla de oro)

| Tipo de operación | ¿CommandQueue? | Por qué |
|---|---|---|
| Escribir memoria del char (name, health, faction, stats, equip, pos) | **SÍ** | El hilo de juego puede estar leyéndola |
| Llamar funciones nativas (`ApplyDamage`, `CharacterDeath`, `CharacterKO`, `addRelation`) | **SÍ** | Mutan estado global del motor, no reentrantes desde otro hilo |
| `MarkRemoteControlled` / sonda de anim / GameData | **SÍ** | Tocan estructuras Ogre/AI del char |
| `registry.*` (Register/SetGameObject/UpdatePosition/UpdateLimbHealth/GetInfo) | NO | Tienen su propio shared/unique lock |
| `Interpolation::AddSnapshot` | NO | Mutex propio |
| `PendingSnapshotQueue`, `VisualProxy`, `PlayerController` tracking | NO | Estado del mod, no del motor |
| `SendReliable` / construir paquetes (PacketWriter) | NO desde hook MovRaxRsp | Usa el ring SPSC del combate; desde hilo de red normal SÍ es seguro |

### 2.3 Dos colas distintas — no confundirlas (corrige una imprecisión de audit-10)

- **`GameCommandQueue`**: red → juego. Para los handlers de paquetes. **Es el que hay que usar para F0/F1.**
- **Ring SPSC de `combat_hooks`**: hook (game thread) → game tick (game thread). Productor y consumidor
  son el MISMO hilo. NO usar para red→juego (no hay sincronización cross-thread; rompería).

El camino C2S del combate (Death/KO detectados por hook → enviar al server) usa el ring SPSC y está
bien. El camino S2C (server → aplicar al motor) es el que necesita el CommandQueue, y CombatHit/Death
ya lo hacen (packet_handler:958, 1032). KO y el resto **no** — eso es F1.

---

## 3. El crash del 2º jugador: faction-UAF (BUG 18) y spawns en bucle (C-3)

### 3.1 BUG 18 — `SEH_AllyModFaction` reintroduce el faction-UAF

`SEH_AllyModFaction` (packet_handler.cpp:66-88, llamado en :736). Lee `accessor.GetFactionPtr()` del
char **remoto** (char+0x10, offset de campo CONFIRMADO) y llama `addRelation(origFn)` con ese puntero,
desde el hilo de red. El guard `remoteFaction < 0x10000 || >= 0x7FFF...` **no detecta una facción ya
liberada** (un puntero a memoria liberada sigue "pareciendo válido"). Cuando la zona del NPC origen se
descarga, el motor accede a `faction+0x250` (offset CONFIRMADO = `PlayerInterface*`) sobre un puntero
colgante → crash documentado en **`game+0x927E94`** (puntero 32-bit sign-extended → prefijo
`0xFFFFFFFF`).

Esto es exactamente el UAF que `PlayerController::OnRemoteCharacterSpawned` (player_controller.cpp:159-167)
ya había **deshabilitado a propósito** ("Faction write DISABLED — use-after-free prevention"). El
`SEH_AllyModFaction` lo reintrodujo por la puerta de atrás.

**Fix (orden de preferencia):**
1. **Inmediato:** desactivar `SEH_AllyModFaction` (no llamarlo). El char remoto queda con su facción
   de fábrica; el nameplate sale neutral en vez de verde — cosmético, no crashea.
2. **Correcto:** no usar el faction-ptr del **remoto** como destino. Para aliar, usar la **player
   faction local** (`PlayerController::GetLocalFactionPtr()`, ya capturada y validada como heap no-módulo
   en player_controller.cpp:31-35) y, si se quiere relación bidireccional, validar el segundo puntero
   contra la **lista viva de facciones** del mundo antes de pasarlo a `addRelation`. Y ejecutarlo desde
   el CommandQueue (game thread), nunca desde red.
3. **Aviso RE:** `Faction+0x08` NO es un "id uint32" (es `_antiSlavery`); identificar facción por
   `name` (`Faction+0x1A8`, CONFIRMADO) o por puntero. El `factionId` que el mod envía en algunos
   paquetes puede ser basura — ligado a "NPCs huyen" (audit-03 §4).

### 3.2 C-2 — votación de facción: race juego/red

`entity_hooks.cpp:148-152` (escritura desde hilo de juego) vs `:1258-1261` (`memset` en
`ResumeForNetwork`, hilo de red), sin mutex. Al conectar el 2º jugador, `ResumeForNetwork` puede pisar
el estado de votación mientras el juego está dentro de esa ventana.
**Fix:** mutex dedicado para el estado de votación, o marshalar `ResumeForNetwork` al game thread vía
CommandQueue (coherente con F0).

### 3.3 A-3 — `SpawnManager` no se resetea en reconnect

`m_factory` y los maps de template (`m_modPlayerTemplates`, `m_characterTemplates`) no se limpian al
desconectar. El primer factory-spawn del remoto tras reconectar puede usar un puntero stale → AV.
**Fix:** método `SpawnManager::ResetForReconnect()` que limpie `m_factory`/templates bajo
`m_templateMutex`, llamado desde el handler de disconnect (junto con `CommandQueue.Clear()`).

### 3.4 C-3 — spawns forzados en bucle (`s_savedChecks`)

`squad_spawn_hooks.cpp:68-136`. Global save/restore por comparación de dirección, sin guard de
reentrancia: si la función del juego se anida con otro platoon, deja `skipCheck1/2` clavados en `false`
→ el motor spawnea sin parar. Se agrava bajo la carga del 2º jugador (más eventos de spawn).
**Fix:** mover el estado a local de pila o `thread_local int depth` (igual que el `s_hookDepth`
ya correcto en entity_hooks). Es el mismo patrón que BUG 7 (`s_replayBackup`).

### 3.5 A-2 — el spawn-cap se satura a 4 (enemigos no spawnean)

`Hook_CharacterDestroy` (entity_hooks.cpp:1203) **no está instalado**; su cuerpo (988-1027) es el único
que decrementa el spawn-cap y limpia interpolación. Nunca corre → el cap llega a 4 y bloquea spawns.
El despawn por paquete (`HandleEntityDespawn`) sí decrementa (:811), pero las muertes/descargas de
zona no. **Fix:** instalar el hook, o invocar `DecrementSpawnCount`+`RemoveEntity`+`Unregister` desde
todas las rutas de destrucción. Relevante para co-op porque el 2º jugador introduce más spawns/muertes.

---

## 4. Sincronización del COMBATE entre jugadores

### 4.1 Lo que YA funciona (modelo actual, confirmado en código)

- **Detección local (C2S)**: `Hook_CharacterDeath`/`Hook_CharacterKO` (combat_hooks.cpp) corren en el
  game thread, hacen lo mínimo (llamar original + push al ring SPSC) y `ProcessDeferredEvents`
  (desde OnGameTick) envía `C2S_CombatDeath`/`C2S_CombatKO` + snapshot de salud (limbs).
- **Aplicación remota (S2C)**: `HandleCombatHit` (958) y `HandleCombatDeath` (1032) ya difieren la
  llamada nativa al **CommandQueue** con echo-suppression (`SetServerSourcedDeath/KO` evita el bucle
  C2S→S2C→C2S). `HandleCombatKO` (1100) **NO difiere** todavía → F1.
- **Salud**: cadena CONFIRMADA `char+0x2B8 → +0x5F8 → +0x40 + part*8` (offsets de campo, estables).
  Sirve de fallback cuando la función nativa falla.
- **Autoridad**: `AuthorityValidator::ValidateInboundSnapshot` rechaza snapshots de quien no es dueño
  de la entidad (owner mismatch). Existe la base de un modelo server-authoritative para posición.

### 4.2 Qué falta para que el combate se vea en AMBOS clientes

1. **F1 — Migrar `HandleCombatKO` y los handlers de salud/stats al CommandQueue** (BUG 19). Hoy
   `HandleCombatKO` (1100-1135) llama `koFn` nativo y escribe la cadena de salud **desde el hilo de
   red**. `HandleHealthUpdate`, `HandleLimbHealth`, `HandleStatUpdate`, `HandleEquipmentUpdate`,
   `HandleInventoryUpdate` igual. Mientras no se difieran, cada hit recibido del server es una ruleta
   de corrupción. Mismo patrón que CombatHit/Death: envolver la escritura/llamada nativa en
   `CommandQueue.Push`, dejando los `registry.UpdateLimbHealth` (lock propio) en el hilo de red.

2. **Modelo autoritativo de daño (decisión de diseño).** Dos opciones:
   - **(a) Autoridad del atacante (más realista a corto plazo).** El cliente que ejecuta el ataque
     detecta hit/KO/death con sus hooks, lo manda al server, el server lo reenvía, y el otro cliente
     lo aplica vía `ApplyDamage`/`CharacterKO`/`CharacterDeath` nativas (difiriendo al CommandQueue).
     Es lo que la arquitectura actual ya soporta. Riesgo: divergencia si los dos motores resuelven el
     combate distinto (cada Kenshi simula su propia IA).
   - **(b) Autoridad pura del server (lo "correcto" pero choca con el muro).** El server simularía el
     combate y mandaría resultados. Kenshi **no expone** un simulador de combate desacoplado del
     cliente: el combate vive dentro del game loop mono-hilo del exe. El server (ENet, sin motor de
     Kenshi) no puede resolver `MartialArtsCombat` (`0x892120`) por su cuenta. → ver §4.4 (el muro).

3. **Replicar daño por intención (requiere P3/P4 de audit-03).** Para que el daño que TÚ infliges se
   vea en el otro cliente con los números correctos, lo ideal es hookear `ApplyDamage` (`0x7A33A0`) y
   reenviar la intención de daño, no solo el resultado de salud. Pero `ApplyDamage` usa `mov rax,rsp`
   y se llama 300+/seg → el wrapper MovRaxRsp con slots **globales** (no TLS) crashea bajo combate
   continuo (audit-03 §2). **Prerequisito: migrar MovRaxRsp a TLS (audit-03 P3, riesgo Alto).** Hasta
   entonces, el sync de combate se queda en el modelo "resultado de salud + death/KO" (menos preciso
   pero estable), no "intención de daño".

### 4.3 El bloqueo del HOST que también afecta al co-op

El combate de 1 jugador está congelado porque el char del host no recibe el AI tick: el gate
`GameWorld+0x8B9` (paused, offset CONFIRMADO) lo manda a la rama "paused" en `mainLoop` (`0x788A00`),
y `paused = arg OR (gameSpeed==0.0)` re-pega el flag si `gameSpeed`(`GameWorld+0x700`) sigue en 0.
**Implicación para el co-op:** mientras el motor del host esté en estado "reloj corre pero personajes
no se simulan", NINGÚN combate (local ni replicado) se verá. El fix de la "pausa fantasma" (audit-03
P0) es **prerequisito** del combate co-op, no independiente de él. El 2º jugador no arregla esto; lo
hereda.

### 4.4 El muro del motor mono-hilo — qué es realista

- **Realista (alcanzable con este plan):**
  - 2º jugador conecta sin crashear (F0/F1/F2). **Esto es lo que desbloquea el plan.**
  - Posición/rotación/animación de los chars remotos interpolada (ya existe; estabilizar con BUG 6/9).
  - Replicar **resultado** de combate: muerte, KO, salud por limb, equipo. Server-authoritative para
    quién-posee-qué (AuthorityValidator). Cada cliente ve al char remoto morir/caer.
  - Daño "best-effort" por intención SI se completa P3 (MovRaxRsp→TLS) + P4 (rehook ApplyDamage).

- **El muro (no resoluble sin reescribir el motor):**
  - **Simulación de combate determinista y idéntica en ambos clientes.** Cada instancia de Kenshi
    corre su PROPIO game loop mono-hilo (`GameWorld::mainLoop`, `updateCharacters` `0x786E30`). No hay
    forma de forzar que dos motores resuelvan el mismo intercambio de golpes igual frame a frame: la
    IA, el RNG de daño, el timing de animaciones y la física Havok divergen. Por eso "nadie ha logrado
    combate sincronizado en Kenshi".
  - **Server-autoritativo puro.** El server no tiene motor de Kenshi → no puede ser árbitro real del
    combate. Lo máximo es **un cliente autoritativo** (el atacante o el host) que decide y los demás
    aplican el resultado. Eso es "combate replicado", no "combate simulado en común".
  - **Lo viable es: cada cliente simula su mundo, el server reconcilia POSICIÓN y EVENTOS DISCRETOS
    (death/KO/hit-result), y se acepta divergencia en los detalles de animación/timing.** Ese es el
    techo realista. Apuntar a "que ambos vean al enemigo morir cuando le pegas", no a "que ambos vean
    la misma coreografía exacta de golpes".

---

## 5. RVAs / offsets relevantes (consolidado de las notas de RE)

> RVA = relativo a ImageBase `0x140000000`, **volátil** (cambia por versión/plataforma; resolver por
> scanner). OFFSET DE CAMPO = **estable** Steam/GOG.

### Funciones (RVA, volátiles)
| Símbolo | RVA | Estado | Uso en el plan |
|---|---|---|---|
| `RootObjectFactory::process` (spawn) | `0x581770` | CONFIRMADO (.pdata) | Ruta B factory |
| `AI::create` | `0x622110` | CONFIRMADO | init de char |
| `ApplyDamage` | `0x7A33A0` | CONFIRMADO | F3/P4 (prólogo mov rax,rsp; 300+/seg) |
| `Character::death` | `0x7A6200` | CONFIRMADO | S2C death (pone char+0x5BC=1) |
| `CharacterKO` | `0x345C10` | **SUPOSICIÓN 🟡** | S2C KO — verificar antes de confiar |
| `addRelation`/`FactionRelation` | `0x872E00` | CONFIRMADO (anchor) | §3 aliado (usar player faction) |
| Crash UAF de facción | `game+0x927E94` | CONFIRMADO (sitio) | el crash que produce SEH_AllyModFaction |
| `GameWorld::mainLoop` | `0x788A00` | CONFIRMADO | gate de simulación (§4.3) |
| `GameWorld::updateCharacters` | `0x786E30` | CONFIRMADO | simulador AI (mov rax,rsp) |
| `GameWorld::setPaused` | `0x787D40` | CONFIRMADO | único escritor de +0x8B9 |
| GameWorld (instancia embebida) | `0x2134110` | CONFIRMADO | NO dereferenciar a ciegas (instancia, no ptr) |

### Offsets de campo (estables)
| Campo | Offset | Estado | Relevancia |
|---|---|---|---|
| `Character.faction` | `+0x10` | CONFIRMADO | el `/verify FAIL` = char no simulado, NO offset mal |
| `Faction.playerInterface` (UAF) | `+0x250` | CONFIRMADO | el campo del crash UAF |
| `Faction.name` | `+0x1A8` | CONFIRMADO | identificar facción (en vez de id) |
| `Faction.id` | `+0x08` | **REFUTADO ❌** | es `_antiSlavery`; NO hay id plano |
| salud chain1 / chain2 / base / stride | `+0x2B8 / +0x5F8 / +0x40 / *8` | CONFIRMADO | health remoto / fallback death/KO |
| `GameWorld.gameSpeed` | `+0x700` | CONFIRMADO | despausar (NO +0xA0) |
| `GameWorld.paused` | `+0x8B9` | CONFIRMADO | gate AI tick (§4.3) |
| `Character.isDead` | `+0x5BC` | CONFIRMADO | NO escribir (mataría al char) |
| `PlayerInterface.participant` / `.playerCharacters` | `+0x2A0` / `+0x2B0` | CONFIRMADO | `controlledChar +0x2A8` (audit-10 M-4) **sin confirmar** |

> **A verificar antes de usar:** `CharacterKO 0x345C10`, `Faction.isPlayerFaction +0x90` (cae en
> `tradeCulture`), `PlayerInterface+0x2A8` (no documentado; lo confirmado es +0x2A0/+0x2B0), y el mítico
> "SimClock+0xA0" (no existe en las notas; el reloj es `GameWorld+0x700` + gate `+0x8B9`).

---

## 6. Plan por fases (resumen accionable)

| Fase | Tarea | Archivos | Riesgo | Desbloquea |
|---|---|---|---|---|
| **F0.1** | Migrar `HandleEntitySpawn` link (669-744) a un solo `CommandQueue.Push` | packet_handler.cpp | Medio | 2º jugador no crashea al spawn |
| **F0.2** | Igual para `ProcessDeferredSpawn` (1865-1939) | packet_handler.cpp | Medio | spawn diferido seguro |
| **F0.3** | Neutralizar `SEH_AllyModFaction` (BUG 18); aliar con player faction local desde CommandQueue | packet_handler.cpp:66-88,736 | Bajo (desactivar) / Medio (sustituir) | elimina el faction-UAF |
| **F0.4** | Mutex en votación de facción (C-2) o marshalar `ResumeForNetwork` | entity_hooks.cpp:148,1258 | Bajo | race de conexión |
| **F0.5** | `SpawnManager::ResetForReconnect` (A-3) + `CommandQueue.Clear()` en disconnect | spawn_manager.cpp, core.h | Bajo | reconexión sin ptr stale |
| **F0.6** | `s_savedChecks` → local/thread_local depth (C-3) | squad_spawn_hooks.cpp:68 | Bajo | spawns en bucle |
| **F0.7** | Instalar/sustituir `Hook_CharacterDestroy` (A-2) | entity_hooks.cpp:1203 | Medio | spawn-cap no se satura |
| **F1** | Migrar KO/Health/Limb/Stat/Equipment/Inventory/Door/FactionRelation al CommandQueue (BUG 19) | packet_handler.cpp (1100,1233,1429,1468,1524,1576,1681,1785) | Medio | estado remoto sin corrupción |
| **F2** | `WaitForFrameWork` + `CommandQueue.Clear()` en TODO teardown (audit-01 BUG 1/2) | core.cpp:1287, sync_orchestrator.cpp:328, core.h | Alto | crashes de disconnect/load |
| **F3** | Combate replicado: completar S2C KO, modelo autoritativo del atacante; (P3 MovRaxRsp→TLS + P4 ApplyDamage para daño por intención) | combat_hooks, mov_rax_rsp_fix | Alto | combate visible en ambos |

**Dependencias externas (heredadas):** el combate co-op (F3) NO se ve hasta que el host salga de la
"pausa fantasma" (audit-03 P0) y los chars reciban el AI tick. F0 es independiente de eso y debe ir
primero — sin 2º jugador estable no hay nada que sincronizar.

---

## 7. Riesgos y verificación

- **Reentrancia del drenado**: el lambda del CommandQueue corre dentro de OnGameTick; si re-encola
  (p.ej. un spawn que dispara otro), va al vector ya swappeado → se procesa el SIGUIENTE tick. Correcto,
  pero vigilar que un init no dependa de que otro init del mismo tick ya haya corrido.
- **Orden de drenado vs pipeline**: el CommandQueue se drena ANTES de interpolación/sync (core.cpp:4985).
  El char remoto queda inicializado antes de que el sync lo lea ese mismo tick. Bien.
- **Capturas por valor obligatorias**: cualquier captura por referencia en un Push es un UAF garantizado
  (el frame del handler de red ya murió). Revisar cada lambda nuevo.
- **No romper Ruta B (factory)**: está bien diseñada; F0 solo toca la Ruta A (link) y la C (deferred).
- **Verificación sugerida (read-only primero):** logs `[DIAG-SIMLIST]`/`[DIAG-THINK]` para confirmar
  que el char remoto entra en la lista de simulación; `/verify` debe pasar `char+0x10` (faction) cuando
  el char está simulado. Probar 2º jugador SIN combate primero (solo spawn + movimiento), confirmar
  cero crashes durante N minutos, y solo entonces F1/F3.
- **MovRaxRsp→TLS (F3/P4) en worktree aislado**: validar con CharacterDeath (baja frecuencia) antes de
  ApplyDamage; fallback a página global si TLS falla.

---

## 8. Conclusión

El crash del 2º jugador **no es un misterio de RE**: es una familia de races red-vs-juego sobre el init
del char remoto, y el patrón de arreglo (`GameCommandQueue`) **ya existe y funciona** en el propio mod.
F0 es trabajo de fontanería de hilos de riesgo contenido (capturar por valor + un Push + revalidar),
no de ingeniería inversa nueva. Los offsets de campo de la cadena (faction `+0x10`, salud
`+0x2B8/+0x5F8/+0x40`, paused `+0x8B9`) están CONFIRMADOS y son estables.

Lo realista a corto plazo: **2º jugador estable, movimiento/animación interpolados, y combate
REPLICADO por eventos discretos (death/KO/hit-result) con autoridad del atacante.** El muro —combate
simulado idéntico frame a frame en ambos motores mono-hilo— no se cruza sin reescribir el motor; el
diseño debe asumir divergencia en los detalles y sincronizar solo posición + eventos.

*Auditoría de solo lectura. Ningún archivo de código fue modificado. Los archivo:línea del mod son
estables; las RVAs del juego se resuelven por scanner.*
