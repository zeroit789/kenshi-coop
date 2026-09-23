// ES: server.h - Declaración del servidor dedicado GameServer y sus estructuras de estado.
//     El servidor NO ejecuta el motor de Kenshi: solo guarda el estado de red (jugadores,
//     entidades, hora, clima) y lo replica a los clientes, que sí ejecutan kenshi_x64.exe con el
//     mod. Es autoritativo: dicta velocidad/pausa, valida posiciones y órdenes, asigna host y
//     facciones. Transporte ENet (UDP) en el puerto 27800 por defecto.
// EN: server.h - Declaration of the GameServer dedicated server and its state structures.
//     The server does NOT run the Kenshi engine: it only keeps the network state (players,
//     entities, time, weather) and replicates it to clients, which do run kenshi_x64.exe with the
//     mod. It is authoritative: it sets speed/pause, validates positions and commands, assigns
//     host and factions. ENet (UDP) transport on port 27800 by default.
#pragma once
#include "kmp/config.h"
#include "kmp/types.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/constants.h"
#include "upnp.h"
#include <enet/enet.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <mutex>
#include <vector>
#include <atomic>

namespace kmp {

// ES: Jugador conectado (tras un handshake válido). La clave en GameServer::m_players es su id.
//     peer = conexión ENet; position/zone = posición de su primera entidad (para zonas de interés);
//     ping = RTT de ENet; lastUpdate = último keepalive (segundos de uptime);
//     ownedEntities = entidades reclamadas al reconectar (no se mantiene al día en otros casos).
// EN: Connected player (after a valid handshake). Its key in GameServer::m_players is its id.
//     peer = ENet connection; position/zone = position of its first entity (for interest zones);
//     ping = ENet RTT; lastUpdate = last keepalive (uptime seconds);
//     ownedEntities = entities reclaimed on reconnect (not kept up to date otherwise).
struct ConnectedPlayer {
    PlayerID    id;
    std::string name;
    ENetPeer*   peer;
    Vec3        position;
    ZoneCoord   zone;
    uint32_t    ping;
    float       lastUpdate;
    std::vector<EntityID> ownedEntities;
    // ES: lobbyReady = pulsó "listo" en el lobby (C2S_LobbyReady); isLoopback = conectado desde
    //     127.0.0.1 (host integrado lanzado por el injector); isReady = ya cargó la partida (C2S_PlayerReady).
    // EN: lobbyReady = pressed "ready" in the lobby (C2S_LobbyReady); isLoopback = connected from
    //     127.0.0.1 (integrated host launched by the injector); isReady = finished loading (C2S_PlayerReady).
    bool        lobbyReady = false;
    bool        isLoopback = false;  // True if this peer connected from 127.0.0.1 (integrated host)
    bool        isReady = false;     // True after C2S_PlayerReady received (game loaded)

    // ES: ── Selector de facciones ──
    // Slot de facción actualmente asignado a este jugador (índice 0-based dentro de
    // m_factionSlots). Se fija en el handshake según factionMode y puede cambiarse en
    // caliente con el comando 'setfaction'. -1 = aún sin asignar.
    // EN: ── Faction selector ──
    //     Faction slot currently assigned to this player (0-based index into m_factionSlots). Set at
    //     handshake time from factionMode and changeable at runtime with 'setfaction'. -1 = unassigned.
    int         factionSlot = -1;

    // ES: ── Rate limiting anti-flood (ventana de 1s por contador) ──
    // Se cuentan mensajes por segundo y se descartan los que excedan el umbral.
    // El loopback (host integrado) queda exento para no estorbar al anfitrión.
    // EN: ── Anti-flood rate limiting (1 s window per counter) ──
    //     Messages per second are counted and those over the threshold are dropped.
    //     The loopback peer (integrated host) is exempt so the host is never throttled.
    float    rateWindowStart = 0.f;  // Inicio de la ventana actual (segundos de uptime)
    uint32_t chatMsgCount    = 0;    // Mensajes de chat en la ventana actual
    uint32_t posMsgCount     = 0;    // Position updates en la ventana actual
};

// ES: Entidad del mundo tal como la conoce el servidor (personaje, edificio, etc.).
//     owner = PlayerID dueño (0 = del servidor/sin dueño, p. ej. tras cargar un guardado o
//     desconectarse su jugador). generation sirve para evitar controlar "fantasmas" reutilizados.
//     health = vida por parte del cuerpo (7), equipment = 14 ranuras de equipo (IDs de plantilla).
// EN: World entity as known by the server (character, building, etc.).
//     owner = owning PlayerID (0 = server-owned/unowned, e.g. after loading a save or when its
//     player disconnects). generation prevents controlling reused "ghost" IDs.
//     health = per-body-part health (7), equipment = 14 equipment slots (template IDs).
struct ServerEntity {
    // ES: Identidad.
    // EN: Identity
    EntityID    id = 0;
    uint32_t    generation = 0; // NEW: Generation for ghost control prevention
    EntityType  type = EntityType::NPC;
    PlayerID    owner = 0;

    // ES: Estado y autoridad. OJO: authority vale Server por defecto y a fecha de este comentario
    //     ningún código del servidor la cambia a Player, pero ServerAuthorityValidator exige Player
    //     (sin verificar en ejecución: probablemente hace que se rechacen posiciones/órdenes de clientes).
    // EN: State & authority (spec §2.2, §2.3)
    //     NOTE: authority defaults to Server and, as of this comment, no server code sets it to Player,
    //     while ServerAuthorityValidator requires Player (not verified at runtime: this probably makes
    //     client position updates/commands get rejected).
    EntityState   state     = EntityState::Active;
    AuthorityType authority = AuthorityType::Server; // Server/Player/Transferring
    uint16_t      dirtyFlags = Dirty_None;

    // ES: Transformación: posición, rotación y zona de la rejilla.
    // EN: Transform
    Vec3        position;
    Quat        rotation;
    ZoneCoord   zone;

    // ES: Estado de juego.
    // EN: Game state
    uint32_t    templateId = 0;
    uint32_t    factionId = 0;
    std::string templateName;   // GameData template name for spawning (e.g. "Greenlander")
    float       health[7] = {100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f};
    // ES: limbHealth = vida de miembros reportada por el cliente (C2S_LimbHealth); statusEffects =
    //     efectos activos (0/1); animState/moveSpeed/flags = datos de animación que se reenvían con la posición.
    // EN: limbHealth = limb health reported by the client (C2S_LimbHealth); statusEffects = active
    //     effects (0/1); animState/moveSpeed/flags = animation data relayed with the position.
    float       limbHealth[7] = {100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f};
    uint8_t     statusEffects[5] = {}; // StatusEffectType -> active (0/1)
    CombatInfo  combat;
    uint8_t     animState = 0;
    uint8_t     moveSpeed = 0;  // 0-255 mapped to 0.0-15.0 m/s
    uint16_t    flags = 0;
    bool        alive = true;
    float       buildProgress = 0.f; // 0.0-1.0 for buildings
    uint32_t    equipment[14] = {};  // EquipSlot::Count = 14
};

// ES: Registro de jugador guardado (persistido entre reinicios): nombre -> IDs de sus entidades,
//     para que al reconectar con el mismo nombre recupere sus entidades.
// EN: Saved player record -- persisted across restarts so reconnecting
// players can reclaim their entities.
struct SavedPlayer {
    std::string name;
    std::vector<EntityID> entityIds;
};

// ES: Servidor de juego. Hilos: el bucle principal llama a Update() (tick); el hilo de consola llama
//     a los métodos públicos. Ambos se serializan con m_mutex (recursive_mutex). Los handlers privados
//     (Handle*) corren siempre dentro de Update(), con el mutex ya tomado.
// EN: Game server. Threads: the main loop calls Update() (tick); the console thread calls the public
//     methods. Both are serialized by m_mutex (recursive_mutex). Private handlers (Handle*) always run
//     inside Update(), with the mutex already held.
class GameServer {
public:
    // ES: Start: inicializa ENet, abre el puerto (UPnP o regla de firewall), crea el host ENet y carga
    //     baneos, facciones y el master server. Stop: apagado ordenado. Update: un tick del servidor.
    // EN: Start: initializes ENet, opens the port (UPnP or firewall rule), creates the ENet host and loads
    //     bans, factions and the master server. Stop: orderly shutdown. Update: one server tick.
    bool Start(const ServerConfig& config);
    void Stop();
    void Update(float deltaTime);

    // ES: Comandos de administración (consola del servidor). Toman m_mutex, salvo LoadWorld,
    //     que se llama una vez al arrancar, antes del bucle de tick.
    // EN: Admin commands
    void KickPlayer(PlayerID id, const std::string& reason);
    void BroadcastSystemMessage(const std::string& message);
    void SaveWorld();
    void LoadWorld();
    void PrintStatus();
    void PrintPlayers();

    // ES: Control de velocidad/pausa global (autoridad del servidor).
    // En MP la velocidad la dicta el SERVIDOR; los clientes la siguen vía TimeSync.
    // Estos métodos los invoca la consola del server (que solo tiene el host = admin).
    // Devuelven true si el cambio fue válido y se aplicó.
    // EN: Global speed/pause control (server authority).
    //     In MP the SERVER sets the speed; clients follow it through TimeSync.
    //     Called from the server console (only the host = admin has it). Return true if applied.
    bool SetGameSpeed(float speed);  // Fija velocidad global (rango 0.0-10.0)
    void PauseWorld();               // Pausa el mundo global (envía speed=0 a los clientes)
    void ResumeWorld();              // Reanuda el mundo a la velocidad configurada
    float GetGameSpeed() const { return m_config.gameSpeed; }
    bool  IsPaused() const { return m_serverPaused; }

    // ES: ── Selector de facciones (autoridad del servidor) ──
    // Igual que velocidad/pausa: lo manda la consola del servidor (host = admin).
    // SetFactionMode: cambia el modo ("single"/"teams"/"per-player") en caliente y lo
    //   persiste en server.json. Reasigna las facciones de todos los jugadores conectados
    //   y se las notifica vía el paquete S2C_FactionAssignment. Devuelve false si el modo
    //   no es válido.
    // EN: ── Faction selector (server authority) ──
    //     Same as speed/pause: driven by the server console (host = admin).
    //     SetFactionMode: changes the mode ("single"/"teams"/"per-player") at runtime and persists it
    //     to server.json. Reassigns every connected player's faction and notifies them through the
    //     S2C_FactionAssignment packet. Returns false if the mode is invalid.
    bool SetFactionMode(const std::string& mode);
    // ES: SetPlayerFaction: asigna manualmente al jugador 'id' la facción del slot indicado
    //   (slot 1-based, 1..N facciones del manifiesto). Notifica al cliente vía S2C.
    //   Devuelve false si el jugador no existe o el slot está fuera de rango.
    // EN: SetPlayerFaction: manually assigns player 'id' the faction of the given slot
    //     (1-based, 1..N manifest factions). Notifies the client via S2C.
    //     Returns false if the player does not exist or the slot is out of range.
    bool SetPlayerFaction(PlayerID id, int slot1Based);
    // ES: PrintFactions: vuelca por consola las facciones del manifiesto y qué jugador tiene
    //   asignada cada una.
    // EN: PrintFactions: prints the manifest factions and which player holds each one.
    void PrintFactions();
    // ES: Guarda en disco la config actual (server.json). Lo usa SetFactionMode para persistir.
    // EN: Writes the current config (server.json) to disk. Used by SetFactionMode to persist.
    void SaveConfig();
    // ES: Ruta del fichero de config con el que se arrancó el server (para persistir cambios).
    // EN: Path of the config file the server started with (to persist changes).
    void SetConfigPath(const std::string& path) { m_configPath = path; }

private:
    // ES: Red: eventos de ENet (conexión, desconexión, paquete recibido). HandlePacket valida el canal
    //     y despacha por tipo de mensaje C2S al handler correspondiente.
    // EN: Network
    void HandleConnect(ENetPeer* peer);
    void HandleDisconnect(ENetPeer* peer);
    void HandlePacket(ENetPeer* peer, const uint8_t* data, size_t size, int channel);

    // ES: Handlers de mensajes C2S (cliente -> servidor). Cada uno lee su payload, valida (propiedad,
    //     rangos, NaN) y normalmente reenvía el evento al resto de clientes como mensaje S2C.
    // EN: Message handlers
    void HandleHandshake(ENetPeer* peer, PacketReader& reader);
    void HandlePositionUpdate(ConnectedPlayer& player, PacketReader& reader);
    void HandleMoveCommand(ConnectedPlayer& player, PacketReader& reader);
    void HandleAttackIntent(ConnectedPlayer& player, PacketReader& reader);
    void HandleChatMessage(ConnectedPlayer& player, PacketReader& reader);
    void HandleBuildRequest(ConnectedPlayer& player, PacketReader& reader);
    void HandleEntitySpawnReq(ConnectedPlayer& player, PacketReader& reader);
    void HandleEntityDespawnReq(ConnectedPlayer& player, PacketReader& reader);
    void HandleEquipmentUpdate(ConnectedPlayer& player, PacketReader& reader);
    void HandleZoneRequest(ConnectedPlayer& player, PacketReader& reader);
    void HandleItemPickup(ConnectedPlayer& player, PacketReader& reader);
    void HandleItemDrop(ConnectedPlayer& player, PacketReader& reader);
    void HandleTradeRequest(ConnectedPlayer& player, PacketReader& reader);
    void HandleSquadCreate(ConnectedPlayer& player, PacketReader& reader);
    void HandleSquadAddMember(ConnectedPlayer& player, PacketReader& reader);
    void HandleFactionRelation(ConnectedPlayer& player, PacketReader& reader);
    void HandleBuildDismantle(ConnectedPlayer& player, PacketReader& reader);
    void HandleBuildRepair(ConnectedPlayer& player, PacketReader& reader);
    void HandleCombatStance(ConnectedPlayer& player, PacketReader& reader);
    void HandleCombatKO(ConnectedPlayer& player, PacketReader& reader);
    void HandleCombatDeath(ConnectedPlayer& player, PacketReader& reader);
    void HandleLimbHealth(ConnectedPlayer& player, PacketReader& reader);
    void HandleStatusEffect(ConnectedPlayer& player, PacketReader& reader);
    void HandleItemTransfer(ConnectedPlayer& player, PacketReader& reader);
    void HandleDoorInteract(ConnectedPlayer& player, PacketReader& reader);
    void HandleAdminCommand(ConnectedPlayer& player, PacketReader& reader);
    void HandleLobbyReady(ConnectedPlayer& player, PacketReader& reader);
    void HandlePlayerReady(ConnectedPlayer& player);

    // ES: Envío: Broadcast = a todos los peers; BroadcastExcept = a todos menos un jugador; SendTo = a uno.
    // EN: Broadcasting
    void Broadcast(const uint8_t* data, size_t len, int channel, uint32_t flags);
    void BroadcastExcept(PlayerID exclude, const uint8_t* data, size_t len, int channel, uint32_t flags);
    void SendTo(PlayerID id, const uint8_t* data, size_t len, int channel, uint32_t flags);

    // ES: Replicación periódica del estado: posiciones (cada tick), TimeSync (cada 5 s), host,
    //     heartbeat de entidades (cada 5 s, anti-fantasmas) y snapshot completo al entrar un jugador.
    // EN: Game state
    void BroadcastPositions();
    void BroadcastTimeSync();
    void BroadcastHostAssignment();
    void BroadcastEntityHeartbeat();
    void SendWorldSnapshot(ConnectedPlayer& player);

    // ES: Utilidades: buscar jugador por peer o por ID.
    // EN: Helpers
    ConnectedPlayer* GetPlayer(ENetPeer* peer);
    ConnectedPlayer* GetPlayer(PlayerID id);

    // ES: Rate limiting: devuelve true si el jugador puede enviar OTRO mensaje del tipo
    // indicado en la ventana actual; false si ya superó el umbral (mensaje a descartar).
    // 'counter' es el contador del jugador a incrementar; 'maxPerSecond' su umbral.
    // EN: Rate limiting: returns true if the player may send ANOTHER message of that kind in the
    //     current window; false if the threshold was exceeded (message to drop).
    //     'counter' is the player's counter to increment; 'maxPerSecond' its threshold.
    bool CheckRateLimit(ConnectedPlayer& player, uint32_t& counter, uint32_t maxPerSecond);

    // ES: Comprueba si un jugador está baneado (por nombre o IP). Usado en handshake.
    // EN: Checks whether a player is banned (by name or IP). Used during the handshake.
    bool IsBanned(const std::string& name, const ENetAddress& addr) const;
    // ES: NextPlayerId: siguiente ID de jugador (secuencial, empieza en 1). Get*PlayerCount: contadores.
    // EN: NextPlayerId: next player ID (sequential, starts at 1). Get*PlayerCount: counters.
    PlayerID NextPlayerId();
    int GetReadyPlayerCount() const;
    int GetConnectedPlayerCount() const;

    // ES: Consulta del navegador de servidores: responde con info del servidor sin handshake y desconecta.
    // EN: Query handler (responds without handshake for server browser)
    void HandleServerQuery(ENetPeer* peer, PacketReader& reader);

    // ES: ── Helpers del selector de facciones ──
    // Carga el manifiesto faction-slots.json (lista ordenada de StringIds de facción de
    // jugador). Si no existe, cae a los 2 slots históricos (10-/12-) para no romper nada.
    // EN: ── Faction selector helpers ──
    //     Loads the faction-slots.json manifest (ordered list of player faction StringIds). If it does
    //     not exist, falls back to the 2 historical slots (10-/12-) so nothing breaks.
    void LoadFactionSlots();
    // ES: Calcula el slot (0-based, acotado a m_factionSlots) que corresponde a un PlayerID
    // según el factionMode configurado (single/teams/per-player).
    // EN: Computes the slot (0-based, clamped to m_factionSlots) for a PlayerID according to the
    //     configured factionMode (single/teams/per-player).
    int  ComputeFactionSlot(PlayerID id) const;
    // ES: Envía el paquete S2C_FactionAssignment a un jugador con el slot indicado (0-based).
    // Centraliza el formato del paquete para que handshake y 'setfaction' usen lo mismo.
    // Actualiza player.factionSlot. Devuelve false si el slot está fuera de rango.
    // EN: Sends the S2C_FactionAssignment packet to a player with the given slot (0-based).
    //     Centralizes the packet format so handshake and 'setfaction' share it.
    //     Updates player.factionSlot. Returns false if the slot is out of range.
    bool SendFactionAssignment(ConnectedPlayer& player, int slot0Based);

    // ES: Host ENet de escucha, configuración activa y ruta del fichero de configuración.
    // EN: Listening ENet host, active config and config file path.
    ENetHost* m_host = nullptr;
    ServerConfig m_config;
    std::string  m_configPath = "server.json"; // Ruta de config para persistir cambios en caliente

    // ES: ── Slots de facción del manifiesto (faction-slots.json) ──
    // Lista ORDENADA de StringIds de facción de jugador ("10-kenshi-online.mod", ...).
    // El índice es el "slot" que se envía al cliente. Se carga en Start().
    // EN: ── Faction slots from the manifest (faction-slots.json) ──
    //     ORDERED list of player faction StringIds ("10-kenshi-online.mod", ...). The index is the
    //     "slot" sent to the client. Loaded in Start().
    std::vector<std::string> m_factionSlots;
    // ES: Estado principal: jugadores conectados, entidades del mundo y jugadores desconectados
    //     con entidades reservadas (nombre -> entidades, se guarda en disco).
    // EN: Main state: connected players, world entities and disconnected players with reserved
    //     entities (name -> entities, persisted to disk).
    std::unordered_map<PlayerID, ConnectedPlayer> m_players;
    std::unordered_map<EntityID, ServerEntity> m_entities;
    std::unordered_map<std::string, SavedPlayer> m_savedPlayers; // name → saved entities (persisted)

    // ES: ── Baneos (comando admin 'ban') ──
    // Conjuntos de nombres e IPs vetados. Se consultan en HandleHandshake para
    // impedir la reconexión de un jugador baneado. Persistidos en bans.json.
    // EN: ── Bans (admin 'ban' command) ──
    //     Sets of banned names and IPs. Checked in HandleHandshake to block a banned player from
    //     reconnecting. Persisted in bans.json.
    std::unordered_set<std::string> m_bannedNames;
    std::unordered_set<std::string> m_bannedIPs;
    void LoadBans();   // Carga bans.json al arrancar
    void SaveBans();   // Persiste el estado de baneos a bans.json

    // ES: Mutex del servidor: lo toma Update() en cada tick y los métodos públicos llamados desde la consola.
    //     Recursivo para que un método público pueda llamar a otro que también lo toma.
    // EN: Server mutex: taken by Update() every tick and by public methods called from the console.
    //     Recursive so a public method can call another one that also locks it.
    std::recursive_mutex m_mutex;

    // ES: Contadores de IDs, host actual (0 = ninguno), tick, reloj del mundo (0-1), clima (0-4),
    //     pausa global y temporizadores de los envíos periódicos.
    // EN: ID counters, current host (0 = none), tick, world clock (0-1), weather (0-4),
    //     global pause and timers for the periodic broadcasts.
    PlayerID m_nextPlayerId = 1;
    EntityID m_nextEntityId = 1;
    uint32_t m_nextSquadId = 0x80000000; // Separate ID space for squads (avoids entity ID collisions)
    PlayerID m_hostPlayerId = 0;  // First connected player = host
    uint32_t m_serverTick = 0;
    float    m_timeOfDay = 0.5f;
    int      m_weatherState = 0;
    bool     m_serverPaused = false;  // Estado de pausa global autoritativo del servidor
    float    m_timeSinceTimeSync = 0.f;
    float    m_timeSinceHeartbeat = 0.f;
    float    m_uptime = 0.f;

    // ES: Mapeo automático de puertos por UPnP (se hace de forma síncrona antes de crear el host ENet).
    // EN: UPnP auto port mapping (runs synchronously before ENet host is created)
    UPnPMapper m_upnp;

    // ES: Limpieza de entidades huérfanas (dueño ya no conectado), cada 30 s.
    // EN: Orphan entity cleanup
    float    m_timeSinceOrphanCleanup = 0.f;

    // ES: Guardado automático del mundo (cada m_autoSaveInterval segundos).
    // EN: Auto-save
    float    m_timeSinceAutoSave = 0.f;
    float    m_autoSaveInterval = 60.f; // seconds

    // ES: Registro en el master server (listado público de servidores). Usa un host ENet aparte;
    //     heartbeat cada 30 s y reconexión con espera exponencial (5 s que se dobla hasta 60 s).
    //     Es opcional: si falla no afecta al juego por IP directa.
    // EN: Master server registration
    ENetHost* m_masterHost = nullptr;  // Separate ENet host for master connection
    ENetPeer* m_masterPeer = nullptr;
    bool      m_masterConnected = false;
    float     m_timeSinceMasterHeartbeat = 0.f;
    float     m_masterHeartbeatInterval = 30.f; // seconds
    float     m_masterReconnectTimer = 0.f;
    float     m_masterReconnectDelay = 5.f;    // seconds, doubles on each failure (max 60s)

    void ConnectToMaster();
    void SendMasterRegister();
    void SendMasterHeartbeat();
    void SendMasterDeregister();
    void UpdateMasterConnection(float deltaTime);
};

// ES: Persistencia del mundo (definida en world_persistence.cpp): guardar/cargar en JSON.
// EN: World persistence (defined in world_persistence.cpp)
bool SaveWorldToFile(const std::string& path,
                     const std::unordered_map<EntityID, ServerEntity>& entities,
                     const std::unordered_map<std::string, SavedPlayer>& savedPlayers,
                     float timeOfDay, int weatherState);
bool LoadWorldFromFile(const std::string& path,
                       std::unordered_map<EntityID, ServerEntity>& entities,
                       std::unordered_map<std::string, SavedPlayer>& savedPlayers,
                       float& timeOfDay, int& weatherState,
                       EntityID& nextEntityId);

} // namespace kmp
