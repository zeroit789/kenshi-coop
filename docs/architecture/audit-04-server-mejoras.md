# Audit 04 — KenshiMP.Server: bugs y mejoras accionables

> Auditoría READ-ONLY (no se ha editado código fuente). Otro agente ya tocó el server
> (commits `dcb0de2`, `eb1951b` + cambios sin commitear: control de velocidad/pausa global,
> ModGen, offsets). Esta auditoría revisa `KenshiMP.Server/` y el protocolo en `KenshiMP.Common/`.
> Fecha: 2026-06-18. Todas las rutas absolutas.
>
> Fuentes leídas: `server.cpp` (2510 líneas), `server.h`, `main.cpp`, `world_persistence.cpp`,
> `KenshiMP.Common/src/config.cpp`, `include/kmp/config.h`, `include/kmp/messages.h`,
> `include/kmp/protocol.h`, `tools/ModGen/ModGen/Program.cs`, `kenshi-online-16.mod`.

---

## RESUMEN EJECUTIVO (prioridades del encargo)

| # | Tema | Estado actual | Severidad | Esfuerzo |
|---|------|---------------|-----------|----------|
| 1 | Facciones de jugador (2 → N) | **Hardcodeado a 2 slots**; ModGen ya genera 16 characters pero NO 16 facciones | Bloqueante para la feature | Medio-Alto (toca .mod + ModGen + server + cliente) |
| 2 | Spam del master server | Sin flag de desactivación cómodo; default apunta a IP de terceros | Cosmético (ensucia log) | Bajo |
| 3a | Robustez world save | Bueno (escritura atómica). 1 bug menor de ruta-fallback | Bajo | Trivial |
| 3b | Validación de paquetes entrantes | Bueno en general; faltan límites de rate y un overflow teórico | Medio | Bajo-Medio |
| 3c | Sistema de admin | Funcional pero sin `ban` real, sin auth, host=admin único | Medio | Medio |

---

## 1. LÍMITE DE 2 FACCIONES → ESCALAR A N (co-op / PvP por elección)

### Diagnóstico exacto

El cuello de botella son **tres capas desincronizadas**:

**Capa A — El servidor (hardcode):**
`E:\Aplicaciones\Kenshi-Online\KenshiMP.Server\server.cpp:689-695`

```cpp
static const char* factionStrings[] = {
    "10-kenshi-online.mod",   // Slot 0 (Player 1)
    "12-kenshi-online.mod",   // Slot 1 (Player 2)
};
static const int numFactions = sizeof(factionStrings) / sizeof(factionStrings[0]);
int slot = (id - 1) % numFactions;   // ← con 2 entradas, Player 3+ reutiliza facción 0/1
```

Con `% numFactions` y 2 entradas, los jugadores 3-16 **comparten** la facción de los jugadores 1-2.
Resultado: P1 y P3 controlan la MISMA facción → no hay PvP entre ellos, se ven como aliados y el
ownership de personajes se solapa. Esta es la causa raíz que la memoria del proyecto etiqueta como
"bug de facciones (enemigos huyen)" combinada con el `fundamental type` (ya arreglado en ModGen).

**Capa B — El .mod (`kenshi-online-16.mod`):**
Verificado leyendo el binario del mod: existen **16 CHARACTERS** ("Player 1".."Player 16") y sus
squads, pero solo **2 FACCIONES** reales ("Player 1", "Player 2"). Los strings numéricos de facción
presentes son `10-`, `11-`, `12-`, `13-`, `19-kenshi-online` — NO hay `14-`..`18-` como facciones de
jugador independientes. `Program.cs:100-121` clona el CHARACTER "Player 1", pero **nunca clona el
record FACTION** ni el SquadTemplate con su facción. Por eso aunque el server enviara `"14-..."` no
existiría esa facción en el FCS y el parche `.rdata` del cliente fallaría silenciosamente.

**Capa C — El cliente:**
El cliente recibe `S2C_FactionAssignment` (string + slot) y parchea ese string en `.rdata` antes de
cargar el save (`lobby_manager.cpp`, descrito en `05-network-protocol.md` §4). Si el string no
corresponde a una facción real del .mod, el jugador queda sin facción válida → aggro roto.

### Propuesta concreta (las 3 capas, en orden)

**Paso 1 — ModGen genera N facciones, no solo N characters.**
`tools/ModGen/ModGen/Program.cs`. Hoy clona el Character "Player 1". Hay que clonar TAMBIÉN el
record `ItemType.Faction` "Player 1" (y su `SquadTemplate`) por cada jugador 3..N, y recablear cada
Character/Squad clonado para apuntar a SU propia facción. Patrón a añadir junto al bucle de `:100`:

```csharp
// Pseudocódigo — clonar la FACCIÓN además del CHARACTER
var faction1 = data.Items.First(i => i.Type == ItemType.Faction && i.Name == "Player 1");
for (int n = 3; n <= TotalPlayers; n++) {
    int fid = nextId++;
    string fStringId = $"{fid}-kenshi-online.mod";
    var fVals = new Dictionary<string, object>(faction1.Values);
    var fRefs = faction1.ReferenceCategories.Select(c => new ReferenceCategory(c)).ToList();
    var facClone = new Item(ItemType.Faction, fid, $"Player {n}", fStringId, faction1.SaveData, fVals, fRefs, ...);
    // fundamental type = 4 (OT_CIVILIAN) ya lo aplica el fix existente; aplicarlo aquí también
    data.Items.Add(facClone);
    // recablear el Character "Player n" y su squad → facClone.StringId
}
```
Y **emitir un manifiesto** `faction-slots.json` (lista ordenada de los `stringId` de facción
generados) para que el server lo lea en vez de hardcodear el array. Esto elimina el acoplamiento.
*Riesgo:* clonar facciones requiere recablear relaciones (faccion↔faccion) o todas las facciones
nuevas saldrán "neutrales entre sí" → no habría PvP por defecto. Hay que decidir la matriz de
relaciones inicial (ver Paso 4).

**Paso 2 — El server lee el manifiesto en vez del array hardcodeado.**
`server.cpp:689`. Sustituir el `static const char* factionStrings[]` por un `std::vector<std::string>
m_factionSlots` cargado al arrancar desde `faction-slots.json` (o desde `ServerConfig`). Quitar el
`% numFactions`: si `id-1 >= m_factionSlots.size()`, rechazar el join con
`S2C_HandshakeReject(reasonCode=3, "No hay slot de facción libre")` en lugar de reutilizar slot.
*Riesgo:* el modulo `%` era lo que "permitía" >2 jugadores sin crashear; al quitarlo, el servidor
debe garantizar que `maxPlayers <= número de facciones`. Validar en `Start()`.

**Paso 3 — Modo de asignación: co-op vs PvP (config).**
Añadir a `ServerConfig` (config.h + config.cpp):
```cpp
std::string factionMode = "per-player"; // "per-player" (cada uno su facción, PvP posible)
                                         // | "teams" (grupos comparten facción, co-op)
                                         // | "single" (todos facción 0, co-op puro estilo actual)
int teamSize = 1;                        // para "teams": jugadores por facción
```
La lógica de `slot` en el handshake pasa a depender de `factionMode`:
- `single` → `slot = 0` siempre (comportamiento co-op puro, sin PvP, el más estable HOY).
- `teams` → `slot = (id-1) / teamSize`.
- `per-player` → `slot = id-1` (requiere `maxPlayers <= numFacciones`).

**Paso 4 — Matriz de relaciones inicial (lo que define co-op vs PvP de verdad).**
Las facciones de jugador necesitan relaciones explícitas. Reusar el mensaje ya existente
`S2C_FactionRelation` (0xC1, server.cpp:1784 `HandleFactionRelation`) que ya valida `-100..+100`:
tras el lobby, el server debería **empujar** la matriz inicial (aliado +100 dentro del equipo,
hostil -100 entre equipos si PvP, neutral si co-op). Hoy ese mensaje solo se reenvía cuando un
cliente lo origina; falta que el server lo emita de forma autoritativa al empezar la partida.

### Escalar a 32

`KMP_MAX_PLAYERS` (`constants.h`) está en 16 y se usa en `enet_host_create(maxPlayers*4)` y en los
clamps de config. Subir a 32 es mecánico (constante + regenerar mod con `TotalPlayers=32`), PERO:
*Riesgo real:* `BroadcastPositions` (server.cpp:1009) tiene el **culling por zona DESACTIVADO** a
propósito ("16 slots no lo necesitan"). Con 32 jugadores × N entidades cada uno × 20 Hz, enviar a
todos TODO puede saturar el `KMP_UPSTREAM_LIMIT` de 2 MB/s. **Antes de 32, reactivar el interest
management por zona** (la infraestructura `ZoneCoord`/`IsAdjacent` ya existe y se usa en el
heartbeat, server.cpp:1097). Recomendación: no saltar a 32 sin medir ancho de banda a 16 primero.

---

## 2. SPAM DEL MASTER SERVER — flag de desactivación limpio

### Diagnóstico

`server.cpp:2294 ConnectToMaster()` YA tiene el early-return correcto:
```cpp
if (m_config.masterServer.empty()) {
    spdlog::info("GameServer: No master server configured, skipping registration");
    return;
}
```
El problema es que el **default NO está vacío**: `config.h:36` y `config.cpp:139` ponen
`masterServer = "162.248.94.149"`. En cuanto se escribe el `server.json` por defecto
(`main.cpp:52`), queda esa IP fija de terceros y el `UpdateMasterConnection` (server.cpp:2400)
reintenta con backoff exponencial (5→10→20→40→60s) eternamente, llenando el log de
`spdlog::warn("Disconnected from master server — will retry...")` en partida local.

### Propuesta (mínima fricción, dos capas)

**Opción A (recomendada) — flag booleano explícito en config.**
Añadir a `ServerConfig` (config.h):
```cpp
bool enableMasterServer = false;  // false = partida local/LAN, no registrar en el browser público
```
Cargar/guardar en config.cpp (junto a las líneas 111-112 / 139-140). Y en `ConnectToMaster()`:
```cpp
if (!m_config.enableMasterServer || m_config.masterServer.empty()) {
    spdlog::info("GameServer: Master server deshabilitado (modo local)");
    return;
}
```
*Riesgo:* ninguno. El registro al master es prescindible para juego directo por IP (confirmado en
`07-server.md` §9). Default `false` es lo correcto para el caso de uso de Zero (local).

**Opción B (cero código) — documentar que se ponga `"masterServer": ""` en server.json.**
Ya funciona hoy gracias al early-return. Es la solución inmediata sin recompilar: editar el
`server.json` generado y vaciar el campo. Pero es frágil (cada `server.json` nuevo lo re-rellena).

**Detalle extra:** bajar el log de reconexión de `warn` a `debug`. `server.cpp:2416` usa
`spdlog::warn` para cada fallo de reconexión — aunque se silencie el spam con el flag, conviene que
un master caído no escupa warnings. Cambiar a `spdlog::debug` o limitar a 1 warn cada N intentos.

---

## 3. ROBUSTEZ: world save, validación de paquetes, admin

### 3a. World save (`world_persistence.cpp`) — sólido, 1 bug menor

**BIEN:** escritura atómica con `MoveFileExA(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`
(`:89`), fallback con rotación a `.bak` (`:97-110`), validación de NaN/inf/coords extremas al cargar
(`:155`), cap `KMP_MAX_SYNC_ENTITIES` (`:139`), try/catch en parseo (`:215`). Esto está bien hecho.

**BUG menor — inconsistencia de ruta de fallback.**
`server.cpp:1537` (LoadWorld) y `:1560` (SaveWorld) usan, si `savePath` está vacío:
```cpp
std::string savePath = m_config.savePath.empty() ? "kenshi_mp_world.json" : m_config.savePath;
```
Pero el default real de `savePath` es `"world.kmpsave"` (config.h:32). El hardcode
`"kenshi_mp_world.json"` solo se usaría si alguien vacía `savePath` en el json → cargaría/guardaría
en un fichero DISTINTO al esperado, perdiendo el mundo silenciosamente. *Propuesta:* unificar el
fallback a una constante `KMP_DEFAULT_SAVE_PATH` compartida, o simplemente repetir `"world.kmpsave"`.
*Riesgo:* casi nulo (solo dispara con config corrupta), pero es una pérdida de datos silenciosa.

**Mejora — backups rotados.** Hoy cada save sobrescribe el anterior. Un save corrupto por un bug de
serialización (no de I/O, que ya está cubierto) machaca el bueno. *Propuesta:* mantener N copias
rotadas (`world.kmpsave.1`, `.2`...) cada X auto-saves. Barato y salva partidas.

**Mejora — el auto-save corre bajo `m_mutex` en el hilo de tick.** `Update()` llama `SaveWorld()`
(server.cpp:225) que serializa a JSON y escribe a disco **con el lock tomado y en el hilo de red**.
Con muchas entidades, el `j.dump(2)` + escritura bloquea el tick (los clientes notan un micro-freeze
cada 60s). *Propuesta:* construir el snapshot bajo lock, soltar el lock, y serializar/escribir en un
hilo aparte (o copiar `m_entities` y volcar fuera del lock). *Riesgo:* hay que copiar el estado bajo
lock para no leer estructuras mutando.

### 3b. Validación de paquetes entrantes — buena base, huecos concretos

**BIEN:** validación de canal por tipo de mensaje (`server.cpp:319-337`), sanitización de nombre
(`:587-593`), rechazo de NaN/inf/coords en posiciones (`:801`) y builds (`:954`), authority
validation (`ValidatePositionUpdate`, `CanClientCommandEntity`), `ReadString` rechaza strings
> maxLen y comprueba bounds (`protocol.h:203-211`), distancia máxima de build (`:966`) y de ataque
(`:876`). El `PacketReader` no hace OOB reads (todo pasa por `ReadRaw` con bounds check).

**HUECO 1 — Sin rate limiting / flood protection.** Un cliente modificado puede inundar el servidor
con `C2S_PositionUpdate`, `C2S_ChatMessage` o `C2S_EntitySpawnReq` a máxima velocidad. No hay
límite de mensajes/seg por peer ni de tamaño de chat. `HandleChatMessage` (`:931`) acepta cualquier
string hasta 1024 bytes sin throttle → spam de chat a todos. *Propuesta:* contador por jugador
(mensajes en ventana de 1s) en `ConnectedPlayer`; descartar/kick si excede. Y un cap de longitud de
chat más bajo (256). *Riesgo:* bajo; thresholds generosos no afectan juego legítimo.

**HUECO 2 — `C2S_EntitySpawnReq` confía en el `ownerId` del wire pero usa `player.id`... mayormente.**
`HandleEntitySpawnReq` (`:1113`) lee `ownerId` del paquete (`:1124`) pero luego asigna
`entity.owner = player.id` (`:1177`) — correcto. PERO el broadcast (`:1205`) reenvía `player.id`,
no el `ownerId` leído, así que es consistente. Sin embargo `templateName` se acepta hasta 255 bytes
sin validar contenido (`:1135`); el cliente lo usa para spawnear vía SpawnManager. Un nombre
malicioso podría apuntar a un template que no existe → spawn fallido (no crash, pero ruido).
*Propuesta:* whitelist de prefijos de template válidos, o al menos validar charset.

**HUECO 3 — Overflow teórico en bounds check del PacketReader.**
`protocol.h:207,214`: `if (m_pos + len > m_size)`. `m_pos` y `len` son `size_t`/`uint16_t`; con
`len` acotado a uint16 (máx 65535) y paquetes ENet pequeños no hay overflow real HOY, pero el patrón
`pos + len > size` es frágil si algún `ReadRaw` recibiera un `len` grande (p.ej. `sizeof(struct)` en
32-bit). *Propuesta:* reescribir como `if (len > m_size - m_pos)` (resta sin overflow, `m_pos<=m_size`
siempre se cumple). *Riesgo:* trivial, defensa en profundidad.

**HUECO 4 — `password` declarado pero NUNCA validado.** `config.h:31` define `password`, pero
`HandleHandshake` (`:535`) no lo comprueba en ningún punto. Un servidor "con contraseña" acepta a
cualquiera. *Propuesta:* añadir campo `password` a `MsgHandshake` (hay un `reserved` libre o ampliar
struct + bump de `KMP_PROTOCOL_VERSION`), y rechazar con `reasonCode=3` si no coincide. *Riesgo:*
cambia el wire del handshake → romper compatibilidad de versión (subir protocolo).

### 3c. Sistema de admin — funcional, limitado

`HandleAdminCommand` (`server.cpp:2176`). **BIEN:** valida `player.id == m_hostPlayerId` y rebota
`S2C_AdminResponse(success=0)` a no-hosts (`:2181`). Comandos: kick(0), setTime(2), setWeather(3),
announce(4), setSpeed(5).

**HUECO 1 — `ban` (commandType 1) anunciado pero NO implementado.** El enum de `MsgAdminCommand`
(`messages.h:290`) y la doc dicen `1=ban`, pero el `switch` (`:2197`) no tiene `case 1` → cae en
`default: "Unknown admin command"`. Kick no impide reconexión inmediata (el jugador vuelve a entrar).
*Propuesta:* implementar ban por nombre/IP en un set `m_bannedNames`/`m_bannedIPs`, comprobado en
`HandleConnect` y `HandleHandshake`. Persistir en `bans.json`. *Riesgo:* ban por IP es frágil con
NAT/IP dinámica; combinar con nombre.

**HUECO 2 — Admin atado a un único host.** Solo `m_hostPlayerId` puede usar admin. Si el host se va,
se reasigna automáticamente (`:294-305`) a un jugador arbitrario (`m_players.begin()`), que hereda
poderes de admin sin control. *Propuesta:* lista de admins por nombre en config (`adminNames`), o
una password de admin separada (comando `/login <pass>` vía chat). *Riesgo:* medio; define política
de quién manda al reconectar.

**HUECO 3 — La consola local (stdin) NO es el host.** Los comandos de `main.cpp` (`speed`, `pause`,
`kick`, `say`) operan directamente sobre `GameServer` sin pasar por la validación de host — correcto
para un admin físico del proceso (VPS), pero conviene documentarlo: quien tiene acceso a la consola
del proceso tiene control total, by design.

**HUECO 4 — `kick` no preserva entidades intencionadamente vs disconnect que sí.** Un kick manda
`S2C_HandshakeReject(reason=2)` + `disconnect_later`; cuando el `DISCONNECT` llega,
`HandleDisconnect` (`:255`) preserva las entidades como reconectables (`m_savedPlayers`). Es decir,
un jugador kickeado puede reconectar y RECLAMAR sus entidades (`:614`). Si el kick es por
comportamiento, esto no es lo deseado. *Propuesta:* marcar al jugador como "kicked" y NO guardar sus
entidades en ese path, o purgarlas. *Riesgo:* bajo, pero es un agujero de moderación.

---

## NOTAS DE THREAD-SAFETY (verificado, sin bug nuevo)

El `m_mutex` es `recursive_mutex`. `Update()` lo toma y llama a todos los `Handle*` privados sin
re-bloquear (correcto). Los métodos públicos llamados desde el hilo de consola (`KickPlayer`,
`SetGameSpeed`, `PauseWorld`, `ResumeWorld`, `BroadcastSystemMessage`, `SaveWorld`) SÍ toman el lock
— verificado. **Atención para futuros agentes:** `BroadcastSystemMessage` (`:1488`) toma el lock y es
llamado desde dentro de `PauseWorld`/`ResumeWorld` que YA tienen el lock → funciona solo porque el
mutex es recursivo. Si alguien cambia a `std::mutex` no-recursivo, deadlock inmediato. Documentado.

---

## ORDEN DE ATAQUE SUGERIDO

1. **Punto 2 (master flag)** — 20 min, cero riesgo, limpia el log YA. Opción A.
2. **Punto 3a bug de ruta** — 5 min, evita pérdida silenciosa de save.
3. **Punto 1 modo `single`/`teams`/`per-player` en config** — primero el switch de modo en el
   server (sin tocar el .mod): con `factionMode="single"` se estabiliza el co-op actual. Luego
   ModGen para generar las N facciones reales y habilitar `per-player`/PvP.
4. **Punto 3b rate limiting + 3c ban** — endurecer antes de exponer a internet.
5. **Reactivar interest management** ANTES de subir a 32 jugadores.
