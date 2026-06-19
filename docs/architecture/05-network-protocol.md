# 05 — Protocolo de Red (Kenshi Co-op)

> Mapa de la capa de red y el protocolo cliente-servidor del mod.
> Generado por análisis de RE del código fuente. Fuentes principales:
> - `KenshiMP.Common/include/kmp/protocol.h` — enum `MessageType`, `PacketHeader`, `PacketReader/Writer`
> - `KenshiMP.Common/include/kmp/messages.h` — structs de payload de cada mensaje
> - `KenshiMP.Common/include/kmp/types.h` — tipos compartidos (EntityID, Vec3, Quat, enums)
> - `KenshiMP.Common/include/kmp/constants.h` — versión de protocolo, canales, tick rate
> - `KenshiMP.Core/net/client.{h,cpp}` — wrapper ENet del cliente
> - `KenshiMP.Core/net/packet_handler.cpp` — dispatcher de paquetes S2C en el cliente
> - `KenshiMP.Core/net/server_query.{h,cpp}` — query ligera de servidores y master server
> - `KenshiMP.Core/hooks/time_hooks.{h,cpp}` — aplicación de TimeSync (velocidad/tiempo)
>
> NOTA: este documento describe SOLO el lado cliente (este repo). El servidor está en otro
> componente. Las direcciones de los structs son layout de wire (network), no offsets del juego.

---

## 1. Transporte — ENet

- **Librería:** ENet (`lib/enet`), UDP fiable/no-fiable por canales.
- **Versión de protocolo:** `KMP_PROTOCOL_VERSION = 1` (`constants.h`). Se valida en el handshake y en la server query.
- **Puerto por defecto:** `KMP_DEFAULT_PORT = 27800`.
- **Máximo de jugadores:** `KMP_MAX_PLAYERS = 16`.
- **Tick rate de sync:** `KMP_TICK_RATE = 20 Hz` (50 ms por tick).
- **Límites de banda:** subida y bajada a 2 MB/s (`KMP_UPSTREAM_LIMIT` / `KMP_DOWNSTREAM_LIMIT`).
- **Timeout de sesión:** tras conectar se fija `enet_peer_timeout(peer, 0, 30000, 60000)` (30-60 s); el connect timeout es 5 s (`KMP_CONNECT_TIMEOUT_MS`).

### Canales (`KMP_CHANNEL_COUNT = 3`)

| ID | Constante | Modo ENet | Uso |
|----|-----------|-----------|-----|
| 0 | `KMP_CHANNEL_RELIABLE_ORDERED` | Reliable + ordenado | Conexión, mundo, entidades, stats, building, squad, faction, chat, admin |
| 1 | `KMP_CHANNEL_RELIABLE_UNORDERED` | Reliable sin orden | Combate, eventos de pipeline-debug |
| 2 | `KMP_CHANNEL_UNRELIABLE_SEQ` | No fiable + secuenciado | Updates de posición / movimiento |

El cliente envía con tres helpers (`NetworkClient`, `client.cpp`):
- `SendReliable()` → canal 0, `ENET_PACKET_FLAG_RELIABLE`
- `SendReliableUnordered()` → canal 1, `ENET_PACKET_FLAG_RELIABLE`
- `SendUnreliable()` → canal 2, `flags = 0` (no fiable + **secuenciado**; ENet descarta paquetes tardíos/desordenados — evita que posiciones viejas pisen el estado actual).

`m_enetMutex` protege TODAS las operaciones de host/peer de ENet (no es thread-safe). El bombeo de eventos vive en `NetworkClient::Update()` (no bloqueante, `enet_host_service` con timeout 0) y entrega los `RECEIVE` al `PacketCallback` registrado por el `PacketHandler`.

---

## 2. Cabecera de paquete

`#pragma pack(push,1)` — **8 bytes exactos** (`static_assert`):

```cpp
struct PacketHeader {        // protocol.h
    MessageType type;        // +0x00  uint8_t  — tipo de mensaje
    uint8_t     flags;       // +0x01  bit 0 = comprimido (zlib, ver compression.h)
    uint16_t    sequence;    // +0x02  número de secuencia
    uint32_t    timestamp;   // +0x04  server tick
};
```

Tras la cabecera viene el payload específico del mensaje. Serialización little-endian cruda
(`memcpy` directo de structs `#pragma pack(1)`), con strings length-prefixed (`uint16_t` + bytes UTF-8).
`PacketReader::ReadString` rechaza strings > maxLen (1024 por defecto) — defensa básica.

---

## 3. Tabla completa de tipos de paquete (`enum class MessageType : uint8_t`)

Dirección: **C2S** = cliente→servidor, **S2C** = servidor→cliente, **MS** = game server↔master server.

| Code | Nombre | Dir | Canal | Struct payload | Propósito |
|------|--------|-----|-------|----------------|-----------|
| 0x01 | C2S_Handshake | C2S | 0 | `MsgHandshake` | Inicio de sesión: versión protocolo + versión juego + nombre |
| 0x02 | S2C_HandshakeAck | S2C | 0 | `MsgHandshakeAck` | Acepta: asigna `playerId`, tick, hora, clima, aforo |
| 0x03 | S2C_HandshakeReject | S2C | 0 | `MsgHandshakeReject` | Rechaza con `reasonCode` (0=lleno,1=versión,2=ban,3=otro) + texto |
| 0x04 | C2S_Disconnect | C2S | 0 | — | Desconexión limpia |
| 0x05 | S2C_PlayerJoined | S2C | 0 | `MsgPlayerJoined` | Un jugador remoto entró (id + nombre). Se difunde a todos (incluido el propio) |
| 0x06 | S2C_PlayerLeft | S2C | 0 | `MsgPlayerLeft` | Jugador salió (`reason`: 0=disc,1=timeout,2=kick). Limpia sus entidades |
| 0x07 | C2S_Keepalive | C2S | 0 | — (solo header) | Keepalive cada ~5 s (ver `core.cpp`) |
| 0x08 | S2C_KeepaliveAck | S2C | 0 | — | ACK del keepalive (no-op en cliente) |
| 0x09 | C2S_PlayerReady | C2S | 0 | — (solo header) | "Estoy in-game y listo para spawnear" (en `OnGameLoaded`) |
| 0x0A | S2C_AllPlayersReady | S2C | 0 | — | "Todos listos, spawnea ya" → procesa `DeferredSpawnQueue` |
| 0x10 | S2C_WorldSnapshot | S2C | 0 | bulk (ver §6) | Lote de entidades del mundo (u32 count + N entidades) |
| 0x11 | S2C_TimeSync | S2C | 0 | `MsgTimeSync` | **Sincroniza hora, clima y VELOCIDAD de juego** (ver §7) |
| 0x12 | S2C_ZoneData | S2C | 0 | (no manejado aún en cliente) | Datos de zona |
| 0x13 | C2S_ZoneRequest | C2S | 0 | (no implementado en cliente) | Pide datos de una zona |
| 0x14 | S2C_EntityHeartbeat | S2C | 0 | `MsgEntityHeartbeat` | Lista periódica (~5 s) de IDs vivos; cliente limpia "ghosts" |
| 0x15 | C2S_EntityAck | C2S | 0 | — | ACK opcional del heartbeat |
| 0x20 | S2C_EntitySpawn | S2C | 0 | `MsgEntitySpawn` + extensiones | Spawn de entidad remota (ver §5) |
| 0x21 | S2C_EntityDespawn | S2C | 0 | `MsgEntityDespawn` | Despawn (`reason`: 0=normal,1=killed,2=fuera de rango) |
| 0x22 | C2S_EntitySpawnReq | C2S | 0 | (campos sueltos) | Cliente pide registrar su personaje como entidad de red |
| 0x23 | C2S_EntityDespawnReq | C2S | 0 | — | Pide despawn |
| 0x30 | C2S_PositionUpdate | C2S | 2 | `MsgC2SPositionUpdate` + N×`CharacterPosition` | Subo mis posiciones |
| 0x31 | S2C_PositionUpdate | S2C | 2 | `MsgS2CPositionUpdate` + N×`CharacterPosition` | Bajo posiciones de otros (con `sourcePlayer`) |
| 0x32 | C2S_MoveCommand | C2S | 2 | `MsgMoveCommand` | Orden de mover a un destino |
| 0x33 | S2C_MoveCommand | S2C | 2 | `MsgMoveCommand` | Eco de orden de movimiento |
| 0x40 | C2S_AttackIntent | C2S | 1 | `MsgAttackIntent` | Intención de atacar (atacante, objetivo, tipo) |
| 0x41 | S2C_CombatHit | S2C | 1 | `MsgCombatHit` | Golpe resuelto: daño por tipo + salud resultante + bloqueo/KO |
| 0x42 | S2C_CombatBlock | S2C | 1 | `MsgCombatStance` (¡reutilizado!) | **El servidor reutiliza este code para enviar STANCE** (`HandleCombatStance`) |
| 0x43 | S2C_CombatDeath | S2C | 1 | `MsgCombatDeath` | Muerte de entidad (entidad + killer, 0=ambiental) |
| 0x44 | S2C_CombatKO | S2C | 1 | `MsgCombatKO` | KO de entidad |
| 0x45 | C2S_CombatStance | C2S | 1 | `MsgCombatStance` | Cambio de postura de combate |
| 0x46 | C2S_CombatDeath | C2S | 1 | `MsgCombatDeath` | Reporte de muerte local |
| 0x47 | C2S_CombatKO | C2S | 1 | `MsgCombatKO` | Reporte de KO local |
| 0x50 | S2C_StatUpdate | S2C | 1 | `MsgStatUpdate` | Update de una skill/stat (índice 0-22, ver §8) |
| 0x51 | S2C_HealthUpdate | S2C | 1 | `MsgHealthUpdate` | Salud completa (7 partes) + nivel de sangre |
| 0x52 | S2C_EquipmentUpdate | S2C | 1 | `MsgEquipmentUpdate` | Equipo en un slot |
| 0x53 | C2S_EquipmentUpdate | C2S | 1 | `MsgEquipmentUpdate` | Reporte de cambio de equipo |
| 0x54 | C2S_LimbHealth | C2S | 1 | `MsgLimbHealth` | Salud por miembro (subida) |
| 0x55 | S2C_LimbHealth | S2C | 1 | `MsgLimbHealth` | Salud por miembro (7 floats) |
| 0x56 | C2S_StatusEffect | C2S | 1 | `MsgStatusEffect` | Efecto de estado (subida) |
| 0x57 | S2C_StatusEffect | S2C | 1 | `MsgStatusEffect` | Efecto de estado (sangrado/inconsciente/...) |
| 0x60 | C2S_ItemPickup | C2S | 1 | `MsgItemPickup` | Recoger ítem |
| 0x61 | C2S_ItemDrop | C2S | 1 | `MsgItemDrop` | Soltar ítem |
| 0x62 | C2S_ItemTransfer | C2S | 1 | `MsgItemTransfer` | Transferir ítem entre contenedores/personajes |
| 0x63 | S2C_InventoryUpdate | S2C | 1 | `MsgInventoryUpdate` | Cambio de inventario (0=add,1=remove,2=modify) |
| 0x64 | C2S_TradeRequest | C2S | 1 | `MsgTradeRequest` | Petición de comercio |
| 0x65 | S2C_TradeResult | S2C | 1 | `MsgTradeResult` | Resultado de comercio (0=denegado,1=aceptado) |
| 0x70 | C2S_BuildRequest | C2S | 0 | `MsgBuildRequest` | Petición de construir |
| 0x71 | S2C_BuildPlaced | S2C | 0 | `MsgBuildPlaced` | Edificio colocado (se registra como ghost, no se construye nativamente) |
| 0x72 | S2C_BuildProgress | S2C | 0 | `MsgBuildProgress` | Progreso de construcción 0.0-1.0 |
| 0x73 | S2C_BuildDestroyed | S2C | 0 | (id + reason) | Edificio destruido/desmantelado |
| 0x74 | C2S_DoorInteract | C2S | 0 | `MsgDoorInteract` | Interacción con puerta (0=open,1=close,2=lock,3=unlock) |
| 0x75 | S2C_DoorState | S2C | 0 | `MsgDoorState` | Estado de puerta (0=cerrada,1=abierta,2=bloqueada,3=rota) |
| 0x76 | C2S_BuildDismantle | C2S | 0 | `MsgBuildDismantle` | Desmantelar edificio |
| 0x77 | C2S_BuildRepair | C2S | 0 | `MsgBuildRepair` | Reparar edificio |
| 0x80 | C2S_ChatMessage | C2S | 0 | `MsgChatMessage` + string | Mensaje de chat (subida) |
| 0x81 | S2C_ChatMessage | S2C | 0 | u32 senderId + string | Chat de otro jugador (bajada) |
| 0x82 | S2C_SystemMessage | S2C | 0 | u32 (unused) + string | Mensaje de sistema del servidor |
| 0x90 | C2S_AdminCommand | C2S | 0 | `MsgAdminCommand` | **Comando admin: kick/ban/setTime/setWeather/announce/setSpeed** (ver §7) |
| 0x91 | S2C_AdminResponse | S2C | 0 | `MsgAdminResponse` | Respuesta admin (0=denegado,1=ok) + texto |
| 0x92 | S2C_HostAssignment | S2C | 0 | `MsgHostAssignment` | **Identidad del host** (envío inicial + reasignación). Define `IsHost()` |
| 0xA0 | C2S_ServerQuery | C2S | 0 | `MsgServerQuery` | Query ligera (sin handshake) — solo versión protocolo |
| 0xA1 | S2C_ServerInfo | S2C | 0 | `MsgServerInfo` | Info del servidor (aforo, puerto, nombre, pvp) |
| 0xB0 | C2S_SquadCreate | C2S | 0 | `MsgSquadCreate` + string | Crear squad |
| 0xB1 | S2C_SquadCreated | S2C | 0 | (id + netId + string) | Squad creado, asigna `squadNetId` |
| 0xB2 | C2S_SquadAddMember | C2S | 0 | (no impl. cliente) | Añadir miembro a squad |
| 0xB3 | S2C_SquadMemberUpdate | S2C | 0 | `MsgSquadMemberUpdate` | Miembro añadido(0)/eliminado(1) de squad |
| 0xC0 | C2S_FactionRelation | C2S | 0 | `MsgFactionRelation` | Cambio de relación entre facciones (subida) |
| 0xC1 | S2C_FactionRelation | S2C | 0 | `MsgFactionRelation` | Aplica relación entre 2 facciones (-100..+100) |
| 0xD0 | MS_Register | MS | — | `MsgMasterRegister` | Game server → master: registrarse/actualizar |
| 0xD1 | MS_Heartbeat | MS | — | `MsgMasterHeartbeat` | Game server → master: keepalive |
| 0xD2 | MS_Deregister | MS | — | — | Game server → master: apagándose |
| 0xD3 | MS_QueryList | MS | — | — (header) | Cliente → master: pide lista de servidores |
| 0xD4 | MS_ServerList | MS | — | N×`MsgMasterServerEntry` | Master → cliente: lista completa de servidores |
| 0xE0 | C2S_PipelineSnapshot | C2S | 1 | blob | Debug: snapshot periódico de pipeline |
| 0xE1 | S2C_PipelineSnapshot | S2C | 1 | u32 sender + blob | Debug: snapshot reenviado de un peer |
| 0xE2 | C2S_PipelineEvent | C2S | 1 | blob | Debug: lote de eventos de pipeline |
| 0xE3 | S2C_PipelineEvent | S2C | 1 | u32 sender + blob | Debug: eventos reenviados de un peer |
| 0xF0 | S2C_FactionAssignment | S2C | 0 | u16 len + string + i32 slot | **Lobby:** asigna string de facción + slot al cliente |
| 0xF1 | C2S_LobbyReady | C2S | 0 | — | **Lobby:** cliente confirma listo con facción cargada |
| 0xF2 | S2C_LobbyStart | S2C | 0 | u8 playerCount | **Lobby:** orden de empezar/cargar |

> Codes NO manejados explícitamente en el `switch` del `PacketHandler` del cliente (caen en `default`/log):
> `S2C_ZoneData (0x12)`, varios C2S que solo se envían. La mayoría de C2S no tienen handler en cliente porque el cliente no los recibe.

---

## 4. Flujo de conexión (cliente)

Máquina de estados del cliente (`enum class ClientPhase`, `core.h`):

```
Startup → MainMenu → Loading → GameReady → Connecting → Connected
```

| Fase | Significado |
|------|-------------|
| Startup | DLL cargada, pattern scan + hooks instalados. `Present` aún no dispara |
| MainMenu | Splash terminado, usuario en menú. `Present` a alto fps |
| Loading | Usuario pulsó New/Continue/Load. Se detecta por gap >2 s entre `Present` |
| GameReady | Mundo cargado, personajes existen. `OnGameLoaded` disparado. Auto-connect puede proceder. Hook CharacterCreate AÚN deshabilitado |
| Connecting | `ConnectAsync` llamado, esperando handshake |
| Connected | Handshake hecho, hook CharacterCreate habilitado, entidades sincronizando |

### Secuencia de handshake (paso a paso)

1. **TCP/ENet connect.** `NetworkClient::ConnectAsync(addr, port)` (no bloqueante). El evento `ENET_EVENT_TYPE_CONNECT` llega en `Update()`.
2. **Cliente → C2S_Handshake** (`overlay.cpp` ~L131). Construye `MsgHandshake{ protocolVersion=KMP_PROTOCOL_VERSION, playerName, gameVersion* }` y lo envía por `SendReliable` (canal 0).
3. **Servidor → S2C_HandshakeAck** o **S2C_HandshakeReject**.
   - Ack (`HandleHandshakeAck`, packet_handler.cpp L354): guarda `playerId`, `SetConnected(true)`, `TransitionTo(Connected)`. Inicializa `PlayerController`, `SyncOrchestrator`, reinicia `PipelineOrch` con el id real. Reactiva entity hooks SOLO si el juego ya está cargado (si no, difiere a `OnGameLoaded`). Aplica time sync inicial desde el ack (`time_hooks::SetServerTime(timeOfDay, 1.0f)`).
   - Reject (`HandleHandshakeReject`): vuelve a `GameReady` para reintentar, muestra `reasonText`.
4. **Servidor → S2C_HostAssignment** (inmediatamente tras el ack si somos host). `SetLocalHostPlayerId`. `IsHost()` se deriva comparando con `GetLocalPlayerId()`. Se reenvía también en reasignación (cuando el host actual se va).
5. **Cliente → C2S_PlayerReady** cuando el mundo termina de cargar (`Core::OnGameLoaded`, core.cpp L1717): "estoy in-game, listo para spawnear".
6. **Servidor → S2C_AllPlayersReady** cuando TODOS enviaron PlayerReady. El cliente entonces procesa la `DeferredSpawnQueue` (spawns que llegaron durante la carga).
7. **Keepalive:** cada ~5 s el cliente envía `C2S_Keepalive` (core.cpp L2293). El servidor responde `S2C_KeepaliveAck` (no-op).

### Flujo de lobby (paralelo, pre-carga)

- **S2C_FactionAssignment (0xF0):** el servidor asigna un string de facción + slot. `LobbyManager::OnFactionAssigned`. Si estamos en MainMenu/GameReady aplica el "faction patch" en memoria inmediatamente.
- **C2S_LobbyReady (0xF1):** cliente confirma facción cargada.
- **S2C_LobbyStart (0xF2):** el servidor manda empezar; el cliente muestra su número de jugador/slot.

### Server browser / query (sin handshake)

`ServerQueryClient` usa un `ENetHost` **separado** para no interferir con la conexión de juego:
- `QueryServer(addr, port)` → envía `C2S_ServerQuery` (0xA0), espera `S2C_ServerInfo` (0xA1).
- `QueryMasterServer(addr, port)` → envía `MS_QueryList` (0xD3), espera `MS_ServerList` (0xD4) con N×`MsgMasterServerEntry`.

---

## 5. Entity Spawn — formato real en el wire (S2C_EntitySpawn 0x20)

`HandleEntitySpawn` (packet_handler.cpp L523) NO usa `MsgEntitySpawn` con un solo `ReadRaw`;
lee campo a campo y añade extensiones de longitud variable. Layout real:

```
u32  entityId
u8   type            (EntityType)
u32  ownerId         (0 = server-owned / NPC)
u32  templateId
f32  posX, posY, posZ
u32  compressedQuat  (smallest-three, ver Quat::Compress)
u32  factionId
[opcional] u16 nameLen + nameLen bytes  (template name, UTF-8, máx 255)
[opcional] u8 extFlag                   (1 = sigue estado extendido)
           └─ si extFlag==1: 7×f32 health[7] + u8 aliveFlag
```

Lógica de cliente:
- Si `ownerId == localPlayerId` → es nuestra propia entidad confirmada: **remapea** el ID local al ID del servidor (`RemapEntityId`), no spawnea duplicado.
- Si el juego no está listo (`!IsGameLoaded()` o fase < GameReady) → encola en `DeferredSpawnQueue` (se procesa en `S2C_AllPlayersReady`).
- Intenta **enlazar** con un personaje del mod pre-creado ("Player 1".."Player 16" del `kenshi-online.mod`) por slot = `ownerId`. Si lo encuentra: escribe posición/nombre/salud, suprime IA (`ai_hooks::MarkRemoteControlled`), pone facción aliada. Si no: cae al pipeline `SpawnManager::QueueSpawn` (FactoryCreate).

`S2C_WorldSnapshot (0x10)`: `u32 entityCount` seguido de N entidades en el mismo formato fijo (33 bytes + nombre variable). Es esencialmente un lote de spawns.

---

## 6. CharacterPosition — formato de movimiento (canal 2, no fiable)

```cpp
struct CharacterPosition {     // messages.h
    EntityID entityId;         // +0x00
    uint32_t generation;       // +0x04  anti-ghost (IDs reusados)
    float    posX, posY, posZ; // +0x08
    uint32_t compressedQuat;   // +0x14  rotación smallest-three
    uint8_t  animStateId;      // +0x18
    uint8_t  moveSpeed;        // +0x19  0-255 → 0.0-15.0 m/s
    uint16_t flags;            // +0x1A  bit0=running, bit1=sneaking, bit2=in combat
};
```

- **C2S_PositionUpdate (0x30):** `u8 characterCount` + N×CharacterPosition. (El cliente también tiene un fast-path single en core.cpp L3223.)
- **S2C_PositionUpdate (0x31):** `u32 sourcePlayer` + `u8 count` + N×CharacterPosition.

`HandlePositionUpdate` (L824): valida NaN/Inf, pasa cada snapshot por `AuthorityValidator::ValidateInboundSnapshot` que devuelve `SnapshotDecision`:
- `ApplyRemote` → `Interpolation::AddSnapshot` (interpolación con buffer ~100 ms).
- `ReconcileLocal` → reservado para reconciliación de predicción (Phase 7, hoy se omite para evitar rubber-banding).
- `QueuePendingSpawn` → encola en `PendingSnapshotQueue` (update llegó antes que el spawn).
- otro → rechaza.

El movimiento NO se aplica llamando a la función nativa MoveTo (el pattern scanner encontró una dirección mid-function, insegura). En su lugar se usa el sistema de interpolación.

Compresión de cuaterniones: **smallest-three** (`Quat::Compress`/`Decompress` en types.h): se descarta la componente mayor, las 3 restantes se cuantizan a 10 bits cada una + 2 bits de índice = 32 bits.

---

## 7. TimeSync y velocidad autoritativa del servidor — CLAVE

### Mensaje S2C_TimeSync (0x11)

```cpp
struct MsgTimeSync {       // messages.h
    uint32_t serverTick;   // tick del servidor
    float    timeOfDay;    // 0.0 a 1.0
    int32_t  weatherState;
    float    gameSpeed;    // 0.1 a 10.0
};
```

### Cómo se procesa en el cliente

`HandleTimeSync` (packet_handler.cpp L1239) hace una sola cosa relevante:

```cpp
time_hooks::SetServerTime(msg.timeOfDay, msg.gameSpeed);
```

`time_hooks::SetServerTime` (time_hooks.cpp L20):
1. Guarda `s_serverTimeOfDay`, `s_serverGameSpeed`, marca `s_hasServerTime = true`.
2. Si ya capturó el puntero al **TimeManager** del juego, escribe DIRECTO a memoria:
   - `timeManager + 0x08` = `timeOfDay`
   - `timeManager + 0x10` = `gameSpeed`

### Cómo se aplica la velocidad cada frame

El hook `Hook_TimeUpdate` (time_hooks.cpp L55) — instalado sobre la función del juego `TimeUpdate`
(RVA `0x214B50`, confirmada UNIQUE en notas RE) — captura el puntero al TimeManager en la
primera llamada y, para **clientes no-host conectados**, escala el delta time entrante:

```cpp
if (core.IsConnected() && !core.IsHost() && s_hasServerTime) {
    deltaTime *= s_serverGameSpeed;   // el cliente avanza al ritmo del servidor
}
```

Tras llamar al trampolín original (con SEH), dispara `core.OnGameTick(deltaTime)`.

> AVISO de RE importante (del propio código): en builds Steam, `TimeUpdate` (0x214B50)
> **NUNCA es llamada por el juego**. El driver real de `OnGameTick` es el hook de
> `Present` en `render_hooks`. Por eso `time_hooks::Install` NO reclama estar "activo".
> Hay un dedup guard de 4 ms en `OnGameTick` por si `TimeUpdate` empezara a dispararse.
>
> **Implicación para la feature de velocidad autoritativa:** la escritura directa a
> `timeManager+0x10` en `SetServerTime` es la vía fiable de imponer la velocidad en el
> cliente (no depende de que `TimeUpdate` se ejecute). El escalado de `deltaTime` en el
> hook es un refuerzo, pero hoy en la práctica está muerto en Steam. Conviene verificar
> que la escritura a `+0x10` no es pisada por el loop interno del juego entre frames; si
> lo fuera, habría que reescribirla periódicamente (p. ej. cada tick desde `OnGameTick`)
> o desde el propio `Present`.

### Accesores de tiempo expuestos (`time_hooks.h`)

| Función | Lee/Escribe | Offset |
|---------|-------------|--------|
| `GetTimeOfDay()` | lee | `timeManager + 0x08` |
| `GetGameSpeed()` | lee | `timeManager + 0x10` |
| `WriteTimeOfDay(f)` | escribe | `timeManager + 0x08` |
| `HasTimeManager()` | — | true si el hook capturó el puntero |

### Comando admin de velocidad (`/gamespeed`)

Lado cliente-host, `builtin_commands.cpp` L417:
```cpp
MsgAdminCommand msg{};
msg.commandType = 5;        // setSpeed
msg.floatParam  = newSpeed; // validado 0.1 .. 10.0
// header C2S_AdminCommand (0x90) + SendReliable
```
Requiere `IsHost()`. El servidor debería validar y rebotar la velocidad a todos vía `S2C_TimeSync`
con el `gameSpeed` nuevo, cerrando el bucle autoritativo. `MsgAdminCommand::commandType`:
0=kick, 1=ban, 2=setTime, 3=setWeather, 4=announce, **5=setSpeed**.

---

## 8. StatUpdate — mapa de índice de stat (S2C_StatUpdate 0x50)

`MsgStatUpdate{ entityId, u8 statIndex, f32 statValue }`. `statValue`: parte entera = nivel,
decimal = % XP. `HandleStatUpdate` (L1182) mapea `statIndex` 0-22 a offsets de `StatsOffsets`:

| idx | stat | idx | stat | idx | stat |
|-----|------|-----|------|-----|------|
| 0 | meleeAttack | 8 | crossbows | 16 | engineering |
| 1 | meleeDefence | 9 | turrets | 17 | medic |
| 2 | dodge | 10 | precision | 18 | farming |
| 3 | martialArts | 11 | stealth | 19 | cooking |
| 4 | strength | 12 | assassination | 20 | weaponsmith |
| 5 | toughness | 13 | lockpicking | 21 | armoursmith |
| 6 | dexterity | 14 | thievery | 22 | labouring |
| 7 | athletics | 15 | science | | |

---

## 9. Particionado de IDs de entidad (`constants.h`, spec §3.6)

| Rango | Tipo |
|-------|------|
| 1 – 255 | Player |
| 256 – 8191 | NPC |
| 8192 – 16383 | Building |
| 16384 – 24575 | Container |
| 24576 – 32767 | Squad |

`NetEntityId{ id, generation }` (types.h): la `generation` invalida paquetes obsoletos cuando
un ID se reutiliza, evitando el bug de "control fantasma" (un paquete viejo del jugador A llega
cuando el ID 55 ya es un NPC).

---

## 10. Notas de seguridad / robustez observadas

- Todo handler de S2C que escribe en memoria del juego se hace tras `IsGameLoaded()` y, en
  muchos casos, **en cola al hilo de juego** (`GetCommandQueue().Push(...)`) — separa hilo de
  red de hilo de lógica. Las llamadas a funciones nativas (ApplyDamage, CharacterDeath,
  CharacterKO, FactionRelation) van envueltas en `__try/__except` (SEH) con fallback a
  escritura directa de memoria.
- Buffers `char[]` recibidos del servidor se null-terminan a mano antes de usarlos como string
  (reasonText, playerName, responseText) — el servidor puede mandar el buffer lleno sin `\0`.
- Flags de "server-sourced" (`combat_hooks::SetServerSourced*`, `faction_hooks::SetServerSourced`)
  evitan bucles de feedback: cuando el cliente aplica un evento del servidor, el hook
  correspondiente no lo re-encola como nuevo evento C2S.
- Banda y aforo dimensionados para 16 jugadores × 20 Hz.

---

## 11. Pendiente / verificar (gaps)

- `S2C_ZoneData (0x12)` y `C2S_ZoneRequest (0x13)` definidos pero sin handler de cliente real.
- `S2C_BuildPlaced` solo registra ghost; no construye nativamente (faltan punteros de world/template).
- Reconciliación de predicción local (`ReconcileLocal`) es un TODO (Phase 7).
- Verificar que `timeManager+0x10` (gameSpeed) no es sobrescrito por el loop del juego entre
  frames; si lo es, reaplicar la velocidad periódicamente para la feature autoritativa.
- `S2C_PositionUpdate` con `sourcePlayer == 0` = broadcast del servidor; sourcePlayer != 0 = de un
  jugador concreto (se usa para alimentar `shared_save_sync` solo de la 1ª entidad de cada peer).
```
