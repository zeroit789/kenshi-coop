# 07 — KenshiMP.Server (Servidor dedicado)

> Mapa de arquitectura del servidor dedicado de Kenshi Co-op.
> Generado por análisis estático del código en `KenshiMP.Server/`.
> Fecha: 2026-06-18. Todas las rutas son absolutas.

El servidor es un ejecutable headless (consola) escrito en C++ que usa **ENet**
(UDP fiable) para el transporte y **spdlog** para logging. Es **autoritativo**: dicta
velocidad/pausa del mundo, valida posiciones y asigna facciones e identidad de host.
NO ejecuta el motor de Kenshi — solo mantiene el estado de red (jugadores, entidades,
hora del mundo) y lo replica a los clientes (que sí corren `kenshi_x64.exe` parcheado).

---

## 0. Índice de archivos (`E:\Aplicaciones\Kenshi-Online\KenshiMP.Server\`)

| Archivo | Responsabilidad |
|---|---|
| `main.cpp` | Entry point. Logging, carga de config, signal handlers, **loop principal de tick** y **loop de consola stdin** (status/players/kick/say/speed/pause/resume/save/stop). |
| `server.h` | Declaración de `GameServer`, structs `ConnectedPlayer`, `ServerEntity`, `SavedPlayer`. Firmas de todos los handlers. |
| `server.cpp` | **~2500 líneas.** Implementación de `GameServer`: ENet, handshake, todos los handlers C2S, broadcasting, TimeSync, host assignment, snapshots, comandos admin, master server. |
| `world_persistence.cpp` | `SaveWorldToFile` / `LoadWorldFromFile` — serialización JSON del mundo (escritura atómica). |
| `upnp.cpp` / `upnp.h` | `UPnPMapper` — abre puerto en el router vía COM `IUPnPNAT`. Fallback a regla de firewall. |
| `combat_resolver.cpp` | `ResolveCombat()` — resolución de daño server-side. |
| `authority_validator.cpp` / `.h` | `ServerAuthorityValidator` — valida que un cliente solo comande entidades que posee. |
| `entity_manager`, `player_manager`, `zone_manager`, `game_state` | Managers auxiliares (.cpp/.h). |
| `CMakeLists.txt` | Target `KenshiMP.Server`. Copia el `.exe` al dir de Kenshi como `KenshiMP.Server.exe` para que el botón "Host" lo encuentre. |

> **MasterServer.exe es un proyecto SEPARADO**: `E:\Aplicaciones\Kenshi-Online\KenshiMP.MasterServer\main.cpp`. Ver §9.

---

## 1. main.cpp — Arranque y loops

### Secuencia de arranque (`main`)
1. **UTF-8** en consola Windows (`SetConsoleOutputCP/CP(CP_UTF8)`) → logs con CJK correctos.
2. Logging dual: consola color + fichero `KenshiOnline_Server.log` (truncado en cada arranque).
3. Carga `ServerConfig` desde `server.json` (o `argv[1]`). Si no existe, escribe los defaults.
4. Registra `SignalHandler` para `SIGINT`/`SIGTERM` → pone `g_running=false`.
5. `GameServer server; server.Start(config)` → si falla, return 1.
6. `server.LoadWorld()` — intenta restaurar estado guardado.
7. Lanza **hilo de consola** (lee stdin) y entra en el **loop principal de tick**.
8. Al salir: `SaveWorld()` → `Stop()` → **detach** del hilo de consola (no `join`, porque
   `getline` bloquea en stdin y colgaría el cierre) → `spdlog::shutdown()`.

### Loop principal de tick (`main.cpp:143-156`)
- `tickIntervalMs = 1000 / config.tickRate` (con tickRate=20 → **50 ms**, 20 Hz).
- Cada vez que pasa el intervalo: `deltaTime` real → `server.Update(deltaTime)`.
- `sleep_for(1ms)` entre comprobaciones para no quemar CPU (busy-wait suave).

### Loop de consola / stdin (`main.cpp:79-137`)
Hilo aparte. Lee líneas con `std::getline(std::cin, ...)`. Comandos:

| Comando | Acción | Método invocado |
|---|---|---|
| `help` | Imprime ayuda | (local) |
| `status` | Estado del server | `server.PrintStatus()` |
| `players` | Lista jugadores | `server.PrintPlayers()` |
| `save` | Guarda el mundo | `server.SaveWorld()` |
| `say <msg>` | Mensaje de sistema a todos | `server.BroadcastSystemMessage()` |
| `kick <id>` | Expulsa jugador (parse `stoul`) | `server.KickPlayer()` |
| `speed <v>` | Velocidad global 0-10 (autoritativa) | `server.SetGameSpeed()` |
| `pause` | Pausa global (envía speed=0) | `server.PauseWorld()` |
| `resume` | Reanuda a velocidad configurada | `server.ResumeWorld()` |
| `stop`/`quit`/`exit` | Apaga el servidor | `g_running=false` |

> **Thread safety:** el hilo de consola llama métodos públicos de `GameServer` que adquieren
> `m_mutex` (un `std::recursive_mutex`). El hilo principal lo tiene tomado durante `Update()`.
> El mutex serializa ambos accesos. **PUNTO PARA EL AGENTE QUE AÑADE COMANDOS:** todo método
> público nuevo debe hacer `std::lock_guard lock(m_mutex)` al entrar, igual que `KickPlayer`,
> `SetGameSpeed`, etc. Los handlers privados (`Handle*`) NO bloquean porque ya corren bajo el
> lock de `Update()`.

---

## 2. GameServer — Loop, conexiones y ENet

### `Start(config)` (`server.cpp:34`)
- `enet_initialize()`.
- **Port forwarding ANTES de escuchar**: `m_upnp.AddMapping(port, port, "UDP", ...)`. Si UPnP
  falla → ejecuta `netsh advfirewall firewall add rule ...` (necesita admin) como fallback.
- Crea host ENet: `enet_host_create(addr=ENET_HOST_ANY:port, peerSlots = maxPlayers*4, KMP_CHANNEL_COUNT=3, down/up=2MB/s)`.
  - **Slots ×4**: holgura para estados transitorios de peer en ciclos rápidos de connect/disconnect.
    El límite lógico real lo impone `m_players.size()`.
- `ConnectToMaster()` para registro en el server browser (ver §9).

### `Update(deltaTime)` (`server.cpp:138`) — corazón del servidor, 20 Hz
Adquiere `m_mutex` y:
1. `m_serverTick++`, acumula `m_uptime`.
2. **Pump ENet**: `enet_host_service(timeout=0)` en bucle → despacha CONNECT / RECEIVE / DISCONNECT.
3. **Reloj del mundo**: `effectiveSpeed = paused ? 0 : gameSpeed`. Avanza `m_timeOfDay` en ciclo 24h.
4. `BroadcastPositions()` — **cada tick**.
5. `BroadcastTimeSync()` — **cada 5 s**.
6. `BroadcastEntityHeartbeat()` — **cada 5 s** (anti-ghost).
7. Actualiza pings de jugadores (`peer->roundTripTime`).
8. **Limpieza de huérfanos** — cada 30 s: entidades con `owner != 0` cuyo dueño ya no está
   conectado → broadcast `S2C_EntityDespawn (reason=2)` y borra de `m_entities`.
9. **Auto-save** — cada `m_autoSaveInterval` (60 s) si hay entidades.
10. `UpdateMasterConnection(deltaTime)` — heartbeat/reconnect al master.
11. `enet_host_flush(m_host)` — envía respuestas en el mismo tick (baja latencia).

### Canales ENet (de `constants.h`)
| Canal | Uso |
|---|---|
| `0` RELIABLE_ORDERED | Handshake, eventos críticos, snapshots, chat, TimeSync, host assignment. |
| `1` RELIABLE_UNORDERED | Equipamiento, relay de pipeline debug. |
| `2` UNRELIABLE_SEQ | **Solo** updates de posición (y keepalive). |

`HandlePacket` (`server.cpp:313`) **valida el canal**: rechaza position updates que no lleguen
por el canal 2, y rechaza mensajes fiables que lleguen por el canal 2 (salvo keepalive).

### Conexión / desconexión
- `HandleConnect` (`:238`): rechaza si `m_players.size() >= maxPlayers`. Fija timeout ENet
  (10s min / 15s max → detecta clientes caídos en ~15s). `peer->data = nullptr` hasta el handshake.
- `HandleDisconnect` (`:255`): **preserva entidades para reconexión** (las marca `owner=0` y
  guarda el mapping `nombre → entityIds` en `m_savedPlayers`). Notifica `S2C_PlayerLeft`, borra
  el jugador, **reasigna host** si el que se fue era el host, y emite mensaje de sistema.

### Identidad de peer
`peer->data` guarda el `PlayerID` como `uintptr_t` (no un puntero real). `GetPlayer(peer)`
(`:1457`) lo castea de vuelta y busca en `m_players`. Loopback (127.0.0.1) se detecta con
`addr.host == 0x0100007F` (network byte order).

---

## 3. Lobby, asignación de facción y host assignment

Todo ocurre en **`HandleHandshake`** (`server.cpp:535`):

### Validaciones de entrada
1. Rechaza handshake duplicado si `peer->data != nullptr`.
2. Rechaza si server lleno → `S2C_HandshakeReject (reasonCode=0)`.
3. Rechaza version mismatch (`msg.protocolVersion != KMP_PROTOCOL_VERSION=1`) → `reject reasonCode=1`.
4. **Sanitiza el nombre**: solo ASCII imprimible (32-126), máx `KMP_MAX_NAME_LENGTH=31`. Vacío → "Player".

### Creación de jugador y reconexión
- `NextPlayerId()` (`:1469`, simple `m_nextPlayerId++`).
- Detecta loopback → candidato a host integrado.
- **Reconexión**: si el nombre coincide con un `SavedPlayer`, **reclama** sus entidades
  (`owner = id`) y borra el registro guardado (`:614-628`).

### Asignación de HOST (`:630-649`)
Regla: **loopback siempre gana sobre no-loopback.**
- (a) Si no hay host → este jugador es host (sea loopback o no).
- (b) Si el host actual es no-loopback y este es loopback → **override** (el host integrado del
  injector se impone aunque un jugador LAN ganara la carrera del primer connect).
- (c) En otro caso → se mantiene el host actual.
- Si cambia → `BroadcastHostAssignment()` (`S2C_HostAssignment` con `newHostPlayerId`).

> El **host es el admin**: solo él puede emitir `C2S_AdminCommand` (validado en `:2181`).

### Asignación de FACCIÓN por slot (`:686-717`) — CLAVE
Cada jugador recibe un **string de facción** único de `kenshi-online.mod`. El cliente parchea
ese string en `.rdata` antes de cargar el save para determinar qué facción controla.

```cpp
static const char* factionStrings[] = {
    "10-kenshi-online.mod",   // Slot 0 (Player 1)
    "12-kenshi-online.mod",   // Slot 1 (Player 2)
};
int slot = (id - 1) % numFactions;   // id 1→slot0, id 2→slot1, id 3→slot0...
```

Formato `"{numId}-{modFilename}"` — la extensión `.mod` ES parte de la referencia FCS.
Se envía vía `S2C_FactionAssignment` (len U16 + string + slot I32).

> **LIMITACIÓN ACTUAL:** solo hay **2 slots de facción** definidos, pero el server admite 16
> jugadores. Con `% numFactions`, los jugadores 3+ **reutilizan** las facciones 0/1. Esto
> probablemente se relaciona con el bug conocido de facciones ("enemigos huyen") apuntado en la
> memoria del proyecto. Para más slots hay que añadir más entradas FCS en `kenshi-online.mod` y
> ampliar este array.

### Flujo de lobby completado
- Tras el handshake: `S2C_HandshakeAck` → `S2C_FactionAssignment` → notifica `S2C_PlayerJoined`
  a los demás → envía la lista de jugadores existentes al nuevo → `SendWorldSnapshot()` →
  mensaje de sistema "joined".

### Dos sistemas de "ready" (¡ojo, coexisten!)
1. **Lobby ready** (`HandleLobbyReady`, `:2144`): `C2S_LobbyReady` → marca `player.lobbyReady`.
   Cuando **todos** están ready → `S2C_LobbyStart` (con nº de jugadores). Es el "ready" de la
   pantalla de lobby previa a entrar al juego.
2. **Player ready** (`HandlePlayerReady`, `:2461`): `C2S_PlayerReady` (tras `OnGameLoaded` del
   cliente) → marca `player.isReady`. Cuando `connected > 1 && ready == connected` →
   `S2C_AllPlayersReady`. Es el "ya cargué el mundo y estoy en partida".

---

## 4. World save — `world.kmpsave` / shared save

Implementado en `world_persistence.cpp`. **Formato JSON versión 2.**

### `SaveWorldToFile` (`:21`)
- Serializa: `version`, `timeOfDay`, `weather`, array `entities`, objeto `players`.
- Por entidad: id, type, owner, templateId, factionId, position[3], rotation[4] (quat wxyz),
  alive, health[7], **templateName** (clave para que el cliente spawnee vía SpawnManager),
  equipment[14].
- `players`: mapping `nombre → [entityIds]` para que reconexiones reclamen entidades.
- **Escritura ATÓMICA** (sin ventana de pérdida de datos):
  1. Escribe a `path + ".tmp"`, flush, comprueba `file.good()`.
  2. `MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` — atómico en NTFS.
  3. Fallback si MoveFileExA falla: rota `path → .bak`, `tmp → path`, restaura `.bak` si falla.

### `LoadWorldFromFile` (`:117`)
- Parsea con try/catch (errores de parseo → return false, no crash).
- **Cap de entidades**: corta en `KMP_MAX_SYNC_ENTITIES = 2048`.
- **Valida posiciones**: descarta entidades con NaN/inf/coords > 1.000.000 (evita crash del
  cliente), pero sigue rastreando el max id para no colisionar.
- `owner = 0` en todas las entidades cargadas (nadie las posee hasta que un jugador reconecte).
- Recalcula `nextEntityId = maxId + 1`.

### `GameServer::LoadWorld` / `SaveWorld` (`server.cpp:1536` / `:1555`)
- Ruta = `m_config.savePath` o, si vacía, `"kenshi_mp_world.json"`.
  > **NOTA:** el default real en `config.h` y `server.json` es **`world.kmpsave`**, pero el
  > fallback hardcodeado en `LoadWorld/SaveWorld` es `kenshi_mp_world.json`. Con la config por
  > defecto se usa `world.kmpsave`; el hardcode solo aplicaría si `savePath` quedara vacío.
- `SaveWorld` construye el mapa de jugadores combinando offline (`m_savedPlayers`) + conectados
  (ownership viva de `m_entities`), de modo que el save siempre refleja quién posee qué.
- Es un **shared save** en el sentido de que TODOS los jugadores comparten un único mundo
  autoritativo en el servidor; no hay saves per-jugador, solo el mapping de ownership.

---

## 5. TimeSync — reloj/velocidad enviados a clientes

### `BroadcastTimeSync` (`server.cpp:1055`) — cada 5 s
Envía `S2C_TimeSync` (canal 0, fiable) con `MsgTimeSync`:
- `serverTick`, `timeOfDay`, `weatherState`.
- `gameSpeed = m_serverPaused ? 0.f : m_config.gameSpeed`.

> El **formato del paquete no cambia** al pausar: `gameSpeed` sigue siendo float; solo el VALOR
> es 0. El cliente ya lo parsea como velocidad y se detiene.

### Control autoritativo de velocidad/pausa
El SERVIDOR dicta la velocidad; los clientes la siguen. Dos vías para cambiarla:
1. **Consola del server** (host=admin): `SetGameSpeed` (`:1503`, rango 0-10), `PauseWorld`
   (`:1520`), `ResumeWorld` (`:1528`). Todas hacen `BroadcastTimeSync()` inmediato (push, no
   esperan los 5s).
2. **Comando admin remoto** del host (`HandleAdminCommand` case 5, `:2262`): `gameSpeed` 0.1-10.0.

`m_serverPaused` es el estado de pausa global. `SetGameSpeed(>0)` lo limpia automáticamente.

### `BroadcastEntityHeartbeat` (`:1080`) — cada 5 s
Por jugador, envía `S2C_EntityHeartbeat` con la lista de entity IDs que **deberían** existir en
su zona de interés. El cliente compara con su registro local y limpia ghosts (entidades que el
server ya no conoce: disconnects silenciosos, despawns perdidos). Filtra por: entidad propia, en
zona adyacente, o server-owned (`owner==0`).

### `BroadcastPositions` (`:1009`) — cada tick (canal 2, no fiable)
Por jugador: recopila TODAS las entidades que NO posee y envía sus posiciones comprimidas
(`CharacterPosition`: pos, quat comprimido, animState, moveSpeed, flags, generation). El culling
por zona está **desactivado** a propósito (16 slots no lo necesitan y antes impedía verse).

---

## 6. Consola de comandos (resumen de despacho)

Ya detallada en §1 (stdin) y §3 (ready). Los comandos de **consola local** (admin físico del
proceso) y los **AdminCommand remotos** (host del juego) son rutas distintas pero comparten los
mismos métodos de `GameServer`:

| Consola local (main.cpp) | AdminCommand remoto (case) | Efecto |
|---|---|---|
| `kick <id>` | case 0 (Kick) | Expulsa (envía `S2C_HandshakeReject reason=2`). |
| — | case 2 (Set time) | `m_timeOfDay` + TimeSync. |
| — | case 3 (Set weather) | `m_weatherState` (0-4) + TimeSync. |
| `say <msg>` | case 4 (Announce) | `BroadcastSystemMessage` (remoto: prefijo `[HOST]`). |
| `speed <v>`/`pause`/`resume` | case 5 (Set speed) | Velocidad global + TimeSync. |

`HandleAdminCommand` rechaza con `S2C_AdminResponse (success=0)` si el emisor no es `m_hostPlayerId`.

---

## 7. Authority validation (server.cpp + authority_validator)

`ServerAuthorityValidator` (`authority_validator.h/.cpp`):
- `CanClientCommandEntity(playerId, entityId, entities)` → false si la entidad no existe, no está
  viva, su authority no es `Player`, o el owner no coincide con el cliente.
- `ValidatePositionUpdate(...)` → validación batch para position updates.

`ServerEntity` (server.h:31) lleva `generation` (anti ghost-control), `authority` (Server/Player/
Transferring), `dirtyFlags`, y arrays de health/limbHealth/statusEffects/equipment. Los position
updates además validan NaN/inf/coords extremas en `HandlePositionUpdate` (`:792`).

---

## 8. UPnP (upnp.cpp)

`UPnPMapper` usa COM `IUPnPNAT` / `IStaticPortMappingCollection`:
- `GetLocalIP()`: socket UDP "conectado" a 8.8.8.8 (sin tráfico real) → `getsockname` da la IP de
  ruta local. **No hace `WSACleanup`** (ENet necesita WinSock vivo).
- `AddMapping()`: hasta 3 reintentos de discovery (UPnP es lento). Mapea `externalPort→localIP:internalPort`.
- `RemoveMapping()` en `Stop()`. `GetExternalIP()` lee la IP pública de la propia mapping.
- Si el router no soporta UPnP → `Start()` cae al fallback de regla de firewall (`netsh`).

---

## 9. Master server — registro externo (por qué falla / irrelevante en local)

### Lado servidor de juego (`server.cpp:2294-2456`)
- `ConnectToMaster()`: crea un **host ENet SEPARADO** (`m_masterHost`, 1 peer, 1 canal) y se
  conecta a `m_config.masterServer:masterPort` (default **162.248.94.149:27801**).
- `SendMasterRegister` (`MS_Register`): envía nombre, puerto, jugadores, IP externa (de UPnP).
- `SendMasterHeartbeat` (`MS_Heartbeat`): cada `m_masterHeartbeatInterval=30 s`.
- `SendMasterDeregister` (`MS_Deregister`): en `Stop()`.
- `UpdateMasterConnection`: gestiona connect/disconnect y **reconexión con backoff exponencial**
  (5→10→20→40→máx 60 s).

### Master server propiamente dicho — proyecto SEPARADO
`E:\Aplicaciones\Kenshi-Online\KenshiMP.MasterServer\main.cpp`. Es un **registro centralizado
para el server browser**:
- Escucha ENet en **puerto 27801** (`DEFAULT_MASTER_PORT`), hasta 128 conexiones.
- `g_servers` (`map "ip:port" → RegisteredServer`). Maneja `MS_Register`, `MS_Heartbeat`,
  `MS_Deregister`, `MS_QueryList` (responde `MS_ServerList` a los browsers y los desconecta).
- **Prune** de servidores sin heartbeat en 90 s, cada 30 s.
- Tiene su propia consola (status/stop/help) y `master.json` (port, logFile).

### Por qué falla y por qué es IRRELEVANTE para juego local
- El default `162.248.94.149:27801` es una IP fija de un host de terceros. Si ese host está caído,
  no responde o el firewall lo bloquea → la conexión al master **falla y reintenta indefinidamente**
  con backoff. Esto solo afecta a la visibilidad en el **server browser público**.
- **NO bloquea el juego**: el registro al master corre en un host ENet aparte (`m_masterHost`) y
  todos sus errores son `spdlog::warn`, nunca abortan `Start()` ni el loop. Si el master no está,
  el servidor sigue aceptando conexiones directas por IP:puerto perfectamente.
- En **LAN / partida local** (IP directa, host integrado por el injector vía loopback) el master
  es **prescindible**: el cliente conecta directo a `host:27800`. Para desactivar el ruido del
  master basta con dejar `masterServer` vacío en `server.json` (`ConnectToMaster` hace early-return
  y loguea "No master server configured").

---

## 10. Config (`kmp::ServerConfig`, config.h) y constantes (constants.h)

### ServerConfig (defaults)
| Campo | Default | Notas |
|---|---|---|
| `serverName` | "KenshiMP Server" | |
| `port` | `KMP_DEFAULT_PORT = 27800` | Puerto UDP del juego. |
| `maxPlayers` | `KMP_MAX_PLAYERS = 16` | |
| `password` | "" | (declarado, no se observa enforcement en handshake). |
| `savePath` | `world.kmpsave` | Save del mundo. |
| `tickRate` | `KMP_TICK_RATE = 20` | 20 Hz → tick 50 ms. |
| `pvpEnabled` | `true` | |
| `gameSpeed` | `1.0` | Velocidad global autoritativa. |
| `masterServer` | `162.248.94.149` | Ver §9. |
| `masterPort` | `27801` | |

### Constantes de red (constants.h)
- `KMP_PROTOCOL_VERSION = 1` (mismatch rechaza handshake y registro al master).
- `KMP_MAX_NAME_LENGTH = 31`, `KMP_CHANNEL_COUNT = 3`.
- `KMP_UPSTREAM_LIMIT = KMP_DOWNSTREAM_LIMIT = 2 MB/s`.
- `KMP_MAX_SYNC_ENTITIES = 2048` (cap de entidades totales sincronizadas / cargadas del save).

---

## 11. Notas para quien añada comandos al servidor

1. **Comando de consola nuevo** → añadir rama en el `if/else` de `main.cpp:83-135` y, si necesita
   tocar estado del server, un método público en `GameServer` que tome `std::lock_guard lock(m_mutex)`.
2. **Comando admin remoto nuevo** → añadir `case N` en el switch de `HandleAdminCommand`
   (`server.cpp:2197`). Ya corre bajo el lock de `Update()`, **no** volver a bloquear el mutex
   (es recursivo, pero evita confusión). Recordar enviar `S2C_AdminResponse`.
3. **Mensaje C2S nuevo** → declarar el `MessageType` en `kmp/messages.h`, añadir `case` en
   `HandlePacket` (`server.cpp:343`) que haga `GetPlayer(peer)` y llame a un handler privado.
4. El **mutex es `recursive_mutex`**: métodos públicos llamados desde consola lo toman; handlers
   privados asumen que ya está tomado por `Update()`. No mezclar las dos convenciones.
5. Tras crear cualquier `ENetPacket`, comprobar `nullptr` (patrón repetido en todo el código) y
   loguear `spdlog::error("Failed to create packet ...")` antes de `return`/`continue`.
