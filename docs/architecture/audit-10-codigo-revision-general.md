# Auditoría 10 — Revisión general de código (Kenshi Co-op / KenshiMP.Core)

> Auditoría **SOLO LECTURA** del mod co-op. Fecha: 2026-06-18. NO se modificó código fuente (otro agente editaba `core.cpp` durante esta revisión).
> Cobertura: hooks (entity/combat/faction/squad/building/inventory/ai/save/movement/char_tracker/world), `core.cpp` (OnGameTick, DIAG, fixes, claim del char, SEH/memoria), threading/concurrencia, spawn y sincronización (spawn_manager, sync, net/packet_handler), shared_save_sync, código muerto.
> Cruzado con: `audit-01-core-sync-bugs.md` (16 bugs threading), `COLA-FIXES-CORE.md`, Estado-del-Arte (2º jugador crashea al conectar).
>
> Los bugs 1-16 son los de audit-01 (confirmados, no se renumeran). Los NUEVOS empiezan en 17. Cada hallazgo: archivo:línea + severidad + fix.

---

## TOP por severidad (resumen ejecutivo)

| # | Sev | Archivo:línea | Síntoma | Estado |
|---|-----|---------------|---------|--------|
| 17 | **CRÍTICO** | packet_handler.cpp:669-744 (HandleEntitySpawn) | Init del personaje remoto (rename, GameData, anim, health, faction, MarkRemoteControlled) corre en HILO DE RED sin CommandQueue. Sospechoso nº1 del crash del 2º jugador. | NUEVO |
| 18 | **CRÍTICO** | packet_handler.cpp:66-88 / :736 (SEH_AllyModFaction) | Reintroduce el faction-UAF que OnRemoteCharacterSpawned había deshabilitado; usa el faction-ptr del remoto desde hilo de red; el guard de rango NO detecta faction liberada. | NUEVO |
| C1 | **CRÍTICO** | char_tracker_hooks.cpp:165-184 (FindByName/FindByPtr) | Devuelven puntero al interior de un map protegido; el caller (shared_save_sync) deref sin lock. Puntero colgante por rehash entre hilos. | NUEVO |
| 19 | **CRÍTICO** | packet_handler.cpp (Health/LimbHealth/Stat/Equipment/Inventory/DoorState/CombatKO/FactionRelation/Despawn) | ~9 handlers escriben memoria de juego o llaman funciones nativas desde HILO DE RED sin diferir. CombatHit/Death sí difieren; el resto no se migró. | NUEVO |
| C-1 | **CRÍTICO** | entity_hooks.cpp:408 (s_replayBackup) | Buffer global save-modify-call-restore con reentrancia/2 hilos. Corrupción de reqStruct (confirma audit-01 BUG 7). Además parece código muerto. | CONFIRMA 7 |
| C-2 | **CRÍTICO** | entity_hooks.cpp:148-152 / :1258-1261 | Estado de votación de facción escrito en hilo de juego y reseteado con memset en ResumeForNetwork (HILO DE RED) sin mutex. Race en la conexión del 2º jugador. | NUEVO |
| C-3 | **CRÍTICO** | squad_spawn_hooks.cpp:68 (s_savedChecks) | Global compartido save/restore sin re-entrancy guard; si la función del juego se anida, deja skipCheck1/2 en false permanente. Spawns forzados en bucle. | NUEVO |
| C2 | **CRÍTICO** | shared_save_sync.cpp:436-446 (Step 4 game speed) | Lee/escribe GameWorld por offsets crudos SIN SEH (único bloque del archivo sin try/except). AV no capturado durante carga de zona. | NUEVO |
| 1 | **CRÍTICO** | core.cpp:1313-1318 (OnLoadingGapDetected) | Clear de registry/interp/frameData sin WaitForFrameWork. UAF/heap corruption con workers vivos. | CONFIRMADO |
| 3 | **CRÍTICO** | core.cpp:3728-3732 (dedup OnGameTick) | Guard de 4ms con static no atómico. No es exclusión mutua; doble swap posible. | CONFIRMADO |
| 2/4 | **CRÍTICO** | sync_orchestrator Reset + pending_snapshot_queue | (audit-01) Teardown sin drenar workers / flush de snapshots desordenado. | Ver audit-01 |
| A-1 | **ALTO** | orchestrator.cpp:~222 (SquadSpawnBypass AOB) | El patrón arranca con lea rbp,[rsp-0xD0] (no es entry-point) y el fallback es RVA GOG en build Steam. Hook a dirección arbitraria. Estilo del bug BuyItem invertido. | NUEVO |
| A-2 | **ALTO** | entity_hooks.cpp:1203 (Hook_CharacterDestroy NO instalado) | El único código que decrementa el spawn-cap y limpia interpolación nunca corre. Cap se satura a 4. Enemigos no spawnean. | NUEVO |
| A-3 | **ALTO** | spawn_manager (m_factory/templates) | No se resetean en reconnect; primer factory-spawn del remoto puede usar puntero stale. AV. | NUEVO |
| 23 | **ALTO** | packet_handler.cpp:1681-1696 (HandleFactionRelation) | Itera TODOS los personajes en hilo de red, fuera de SEH, mientras el juego muta la lista. | NUEVO |
| 21 | **ALTO** | ownership.cpp:33 / entity_resolver.cpp:253 | ownerPlayerId == 0 = server-owned colisiona con jugador real 0 (host). | NUEVO |
| A4 | **ALTO** | movement_hooks.cpp:118 vs shared_save_sync.cpp:396 | DOS layouts binarios distintos para el MISMO C2S_PositionUpdate. | NUEVO |
| A3p | **ALTO** | building_hooks.cpp:144-150 | Offsets mágicos +0x28/+0x08 sin verificar (el +0x28 ya fue erróneo en inventory). Hooks SÍ activos. | NUEVO |
| 22/26 | **ALTO** | entity_resolver.cpp:142 / player_engine.cpp:70 | Interés y sesiones de jugadores idos nunca se podan (disconnect ENet directo no emite PlayerLeft). | NUEVO |
| A-4 | **ALTO** | entity_hooks.cpp:100-101 (burst) | s_connectedBurstStart (time_point plano) en RMW no atómico entre render+lógica. | NUEVO |

(MEDIOS y BAJOS detallados abajo; los 16 de audit-01 siguen vigentes.)

---

## El crash del 2º jugador — diagnóstico consolidado

No es un único UAF, sino una **familia de races hilo-de-red vs hilo-de-juego sobre estado global no sincronizado**, que se disparan cuando el 2º jugador conecta mientras el host está en pleno tick:

1. **BUG 17** — HandleEntitySpawn ejecuta TODO el init del personaje remoto (WriteName, GameData, anim probe, limb health, MarkRemoteControlled, faction) en el hilo de red, sin CommandQueue. Solo la POSICIÓN se difiere. El SEH evita que un AV mate el hilo de red, pero no evita corromper estructuras del motor (Ogre/anim/GameData) que el hilo de juego está tocando a la vez.
2. **BUG 18** — SEH_AllyModFaction reintroduce el faction-UAF documentado (player_controller.cpp:159-167): usa el faction-ptr del remoto desde el hilo de red; al descargarse la zona del NPC origen el puntero queda colgante (el guard de rango no lo detecta), crash en faction+0x250 / game+0x927E94.
3. **C-2** — ResumeForNetwork (hilo de red) hace memset del estado de votación de facción mientras el hilo de juego puede estar dentro de esa ventana, sin mutex.
4. **A-3** — SpawnManager::m_factory y los maps de template no se resetean en reconnect; el primer factory-spawn puede usar un puntero stale.

**Arreglo recomendado (orden):** migrar TODO HandleEntitySpawn/ProcessDeferredSpawn a un único CommandQueue.Push (el patrón correcto YA existe en CombatHit/CombatDeath); desactivar o diferir SEH_AllyModFaction; serializar votación de facción + reset con un mutex (o marshalar ResumeForNetwork al hilo de juego); resetear el estado de SpawnManager en disconnect.

---

## CRÍTICOS (detalle de los NUEVOS)

### BUG 17 — packet handler escribe al motor desde el hilo de red (init de spawn)
`net/packet_handler.cpp:669-744` · CONFIRMADO. El bloque link existing mod character llama directo, sin CommandQueue: SetGameObject+MarkRemoteControlled (:671), OnRemoteCharacterSpawned->WriteName (player_controller.cpp:152), WriteNameToGameData (:712), anim probe + WriteLimbHealthToChar (:715-724), SEH_AllyModFaction (:736). Solo la posición (:696) va por CommandQueue.
**FIX:** envolver TODO el init en un CommandQueue.Push para ejecutarlo en el hilo de juego.

### BUG 18 — SEH_AllyModFaction = faction-UAF reintroducido
`net/packet_handler.cpp:66-88` (call en :736). Lee accessor.GetFactionPtr() del remoto (:73) y llama origFn con ese ptr (:80-81), desde hilo de red. El guard menor-que-0x10000 o mayor-igual-0x7FFF NO detecta facción ya liberada. Es el UAF que OnRemoteCharacterSpawned había deshabilitado.
**FIX:** diferir al CommandQueue + no usar el faction-ptr del remoto como destino persistente (capturar la del JUGADOR local) o validar contra la lista viva. Mientras tanto, desactivarlo.

### BUG 19 — Escrituras al motor desde hilo de red (varios handlers)
HandleHealthUpdate (:1429), HandleLimbHealth (:1468), HandleStatUpdate (:1233), HandleEquipmentUpdate (:1524), HandleInventoryUpdate (:1576), HandleDoorState (:1785), HandleCombatKO (:1124, llama koFn nativo directo, a diferencia de CombatHit/Death), HandleFactionRelation (:1683), HandleEntityDespawn (:814). Todos tocan memoria de juego o funciones nativas sin diferir.
**FIX:** mover toda escritura/llamada nativa al CommandQueue. Los updates a registry.* (con su lock) pueden quedarse en hilo de red.

### C1 — char_tracker FindByName/FindByPtr filtran punteros fuera del lock
`char_tracker_hooks.cpp:165-184`. Devuelven puntero al interior de s_trackedChars; el lock_guard muere al salir. Caller (shared_save_sync.cpp:307-324, 360-373) deref tc->animClassPtr/characterPtr sin lock mientras el hilo de anim inserta y rehashea. Puntero colgante.
**FIX:** devolver copia (std::optional) o copiar campos bajo el lock.

### C-2 — votación de facción: race juego/red
`entity_hooks.cpp:148-152` (escritura en hilo de juego) vs `:1258-1261` (memset en ResumeForNetwork, hilo de red). Sin mutex ni atómicas.
**FIX:** mutex dedicado o marshalar ResumeForNetwork al hilo de juego.

### C-3 — squad_spawn s_savedChecks sin re-entrancy guard
`squad_spawn_hooks.cpp:68-136`. save antes del original / restore después por comparación de addr; si el original re-entra con otro platoon, pisa el global. skipCheck1/2 clavados en false (spawn forzado en bucle).
**FIX:** guardar en local de pila o thread_local int depth (como entity_hooks::s_hookDepth).

### C2 — shared_save_sync game-speed sin SEH
`shared_save_sync.cpp:436-446`. GameWorldAccessor + GetGameSpeed/WriteGameSpeed por offsets crudos, sin try/except (resto del archivo sí usa SEH_*).
**FIX:** envolver en SEH_SyncGameSpeed().

### C-1 — s_replayBackup global (confirma audit-01 BUG 7) + probable código muerto
`entity_hooks.cpp:408`.
**FIX:** buffer local de pila. Verificar si SEH_ReplayFactory tiene callers; si no, eliminarlo (flujo activo = NPC-hijack).

---

## ALTOS (detalle de los NUEVOS)

- **A-1** orchestrator.cpp:~222 — AOB de SquadSpawnBypass no apunta a entry-point + fallback RVA GOG en build Steam. **FIX:** validar match con .pdata/RtlLookupFunctionEntry (como validateFactoryFunc); no usar RVA GOG en Steam. Candidato más probable a dirección/firma invertida tipo BuyItem.
- **A-2** entity_hooks.cpp:1203 — Hook_CharacterDestroy NO instalado; su cuerpo (988-1027) tiene el decremento del spawn-cap y la limpieza de interpolación, nunca corre, cap a 4 bloquea spawns. **FIX:** instalarlo o llamar a DecrementSpawnCount+RemoveEntity+Unregister desde el despawn activo (packet_handler).
- **A-3** spawn_manager — resetear m_factory + maps de template en reconnect; serializar m_factory.
- **BUG 23** packet_handler.cpp:1681-1696 — iteración global de personajes en hilo de red sin SEH. **FIX:** diferir todo HandleFactionRelation al CommandQueue.
- **BUG 21** ownership.cpp:33 / entity_resolver.cpp:253 — owner==0 ambiguo (servidor vs jugador 0/host). **FIX:** centinela dedicado (SERVER_OWNER=UINT32_MAX) o flag isServerOwned.
- **A4** movement_hooks.cpp:118 vs shared_save_sync.cpp:396 — dos layouts del mismo packet. **FIX:** unificar el formato C2S_PositionUpdate en un único sitio.
- **A3p** building_hooks.cpp:144-150 — offsets +0x28/+0x08 sin verificar (hooks activos). **FIX:** validar contra KenshiLib antes de confiar.
- **BUG 22/26** entity_resolver.cpp:142 / player_engine.cpp:70 — interés/sesiones no podadas en disconnect ENet directo (client.cpp:154-160 no emite PlayerLeft). **FIX:** TTL/poda + OnPeerTimeout.
- **A-4** entity_hooks.cpp:100-101 — burst (time_point plano) RMW no atómico. **FIX:** mutex o atómicos.
- **A2** inventory_hooks.cpp:182-188 — si SEH_BuyItem falla, retorna 0 sin haber ejecutado la original, posible desincronía dinero/inventario. **FIX:** documentar/validar que la UI no descuenta antes.

---

## MEDIOS

- **BUG 9-14** (audit-01): snap-correction multi-hilo, looksLikePointer, dt fijo 0.016, FORCE BYPASS re-arranca contador (core.cpp:5579-5609), leaks de interpolación/pending. Vigentes.
- **BUG 24** zone_interest.cpp — ZoneInterestManager parece singleton muerto/duplicado de ZoneEngine (dos fuentes de verdad para la zona local). Confirmar uso con grep; si muerto, eliminar.
- **BUG 25** zone_engine.cpp:15-45 — UpdateLocalPlayerZone lee/escribe m_localZone sin tomar m_mutex.
- **M-1** entity_hooks.cpp ResumeForNetwork no resetea offsets detectados del struct (position/gameData). Reuso de offsets de sesión previa con save distinto.
- **M-4** WritePlayerControlled es no-op (offset -2, campo inexistente en 1.0.68) pero se sigue llamando en packet_handler.cpp:500, overlay.cpp:199, sync_orchestrator.cpp:249 y :782 (este pasa true esperando activar control, silenciosamente no hace nada). **FIX:** sustituir por el mecanismo real (igualdad de facción + controlledChar en PI+0x2A8).
- **M3p** building_hooks.cpp:54-65,127 — contadores de crash int plano + HookManager::Remove dentro del propio hook (auto-desinstalarse en medio de su ejecución). Verificar Remove re-entrante.
- **M4p** shared_save_sync.cpp:227,251 — umbral inferior de puntero menor-que-0x10000 muy laxo (squad subió a 0x1000000 por ruido de SSO). Subir por consistencia.

---

## BAJOS / hardening

- **BUG 15** core.cpp:5506-5511 SendCachedPackets — CONFIRMADO código muerto (sin callers; coment. en :4857 lo documenta como removido). Eliminar.
- **BUG 16** entity_registry.cpp overflow de m_nextId si netId==UINT32_MAX.
- **B1** entity_hooks.cpp:735,757-763,827 — spdlog dentro de contexto MovRaxRsp (inconsistente con rutas que usan OutputDebugStringA). Riesgo latente en rutas no-burst.
- **B2** entity_hooks.cpp:1059-1060 — VirtualProtect del stub sin verificar retorno (W^X residual).
- **B3** spawn_manager.cpp:513-516 — ScanGameDataHeap Strategy 1 usa offsets GOG en build Steam.
- **B4** shared_save_sync.cpp:450-456 — OnRemotePosition/GameSpeed sin validar NaN/Inf/rango antes de escribir en memoria de juego (SEH evita crash, no el teleport a NaN).

---

## CÓDIGO MUERTO confirmado (limpiar, NO borrar sin OK de Zero)

| Ubicación | Qué es |
|---|---|
| core.cpp:5506-5511 | SendCachedPackets sin callers (audit-01 BUG 15) |
| save_hooks.cpp:18-50 | SaveGame/LoadGame nunca instalados (mismatch firma); Hook_* + typedefs muertos; IsLoading() siempre false |
| movement_hooks.cpp:198-229 | Hook_SetPosition/Hook_MoveTo nunca instalados (~160 líneas); el bloqueo de MoveTo para remotos NO se aplica |
| squad_hooks.cpp:144-211 | SquadCreate/SquadAddMember nunca hookeados; solo se usa el ptr raw; Uninstall hace Remove sobre nada |
| faction_hooks.cpp (Install) | NO-OP: 0x872E00 es un LOGGER, no el setter (decisión correcta; Hook conservado por si se reancla a addRelation 0x6B2EA0) |
| entity_hooks.cpp:408-428, 1035-1065, 1319-1394 | SEH_ReplayFactory + CallFactoryDirect/CreateRandom + BuildDirectCallStub probablemente muertos (struct-clone REMOVED). El stub RWX vivo sin uso es lo relevante |
| spawn_manager.cpp:286-301 | ProcessSpawnQueue no-op deprecated |
| combat_hooks.cpp:412-422 | DIAG IssueOrder DESHABILITADO (0x722EF0 es UI/MyGUI); Hook_StartAttack conservado |

---

## Lo que está BIEN (no perder tiempo)

- EntityRegistry: locking correcto (shared/unique), GetInfo devuelve copia, sin UAF de metadatos.
- Interpolation: ring bajo mutex, validación NaN/Inf de pos y quat correcta (salvo BUG 9).
- combat_hooks deferred queue: ring SPSC lock-free correcto (productor y consumidor = mismo game thread); el cuerpo del hook hace solo lo mínimo (call original + push), nada de spdlog/PacketWriter/SendReliable en contexto MovRaxRsp. Echo suppression (s_serverSourcedDeath/KO) bien planteada.
- Decisión de NO hookear ApplyDamage (mov rax,rsp + alta frecuencia): correcta y bien documentada.
- Decisión de NO hookear FactionRelation/IssueOrder tras RE: correcta (eran logger/UI).
- FIX-HOSTILITY y FIX-SIMSEED (core.cpp): RE de bytes serio (char+0xD0 / rama cleanup 0x5CCE76, reloj SimClock+0xA0), one-shot re-armable, throttled, solo-host, write de 4 bytes bajo SEH. Sólido.
- ClaimHostPrimaryCharacter: idempotente, sin nombre frágil, faction id con fallback graceful. OK.
- ai_hooks s_remoteControlled y shared_save_sync s_remotePosition: bien protegidos por mutex.
- Firmas verificadas OK: BuyItem (buyer/item/seller tras el fix del invertido), ItemPickup/ItemDrop, inventory.owner +0x88. CharacterCreate + factory validados con .pdata.

---

## Orden de arreglo recomendado

1. Crash del 2º jugador / faction-UAF (BUG 17 + 18 + 19 + C-2): migrar TODO el camino de spawn y los handlers que tocan el motor al CommandQueue (patrón ya existente en CombatHit/Death).
2. Familia threading audit-01 (BUG 1 + 2 + 3): serializar OnGameTick (atómica/mutex) + WaitForFrameWork en TODO teardown (load in-game, disconnect).
3. C1 (char_tracker punteros colgantes), C-3 (squad_spawn re-entrancy), C-1 (s_replayBackup local).
4. A-2 (instalar/sustituir CharacterDestroy, desbloquea el spawn-cap, ligado a enemigos no spawnean).
5. A-1 (validar AOB SquadSpawnBypass con .pdata; quitar fallback GOG en Steam).
6. A4 / A3p / 21 / 22 / 26 (protocolo de posición unificado, offsets building, semántica owner, podas).
7. Resto (medios/bajos) + limpieza de código muerto (con OK de Zero para borrar).

---

*Auditoría de solo lectura. Ningún archivo de código fue modificado. Los archivo:línea de código del mod son estables; las RVAs del juego se resuelven por scanner y no afectan a los bugs de lógica/sincronización.*
