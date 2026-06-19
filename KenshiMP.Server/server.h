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

struct ConnectedPlayer {
    PlayerID    id;
    std::string name;
    ENetPeer*   peer;
    Vec3        position;
    ZoneCoord   zone;
    uint32_t    ping;
    float       lastUpdate;
    std::vector<EntityID> ownedEntities;
    bool        lobbyReady = false;
    bool        isLoopback = false;  // True if this peer connected from 127.0.0.1 (integrated host)
    bool        isReady = false;     // True after C2S_PlayerReady received (game loaded)

    // ── Selector de facciones ──
    // Slot de facción actualmente asignado a este jugador (índice 0-based dentro de
    // m_factionSlots). Se fija en el handshake según factionMode y puede cambiarse en
    // caliente con el comando 'setfaction'. -1 = aún sin asignar.
    int         factionSlot = -1;

    // ── Rate limiting anti-flood (ventana de 1s por contador) ──
    // Se cuentan mensajes por segundo y se descartan los que excedan el umbral.
    // El loopback (host integrado) queda exento para no estorbar al anfitrión.
    float    rateWindowStart = 0.f;  // Inicio de la ventana actual (segundos de uptime)
    uint32_t chatMsgCount    = 0;    // Mensajes de chat en la ventana actual
    uint32_t posMsgCount     = 0;    // Position updates en la ventana actual
};

struct ServerEntity {
    // Identity
    EntityID    id = 0;
    uint32_t    generation = 0; // NEW: Generation for ghost control prevention
    EntityType  type = EntityType::NPC;
    PlayerID    owner = 0;

    // State & authority (spec §2.2, §2.3)
    EntityState   state     = EntityState::Active;
    AuthorityType authority = AuthorityType::Server; // Server/Player/Transferring
    uint16_t      dirtyFlags = Dirty_None;

    // Transform
    Vec3        position;
    Quat        rotation;
    ZoneCoord   zone;

    // Game state
    uint32_t    templateId = 0;
    uint32_t    factionId = 0;
    std::string templateName;   // GameData template name for spawning (e.g. "Greenlander")
    float       health[7] = {100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f};
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

// Saved player record -- persisted across restarts so reconnecting
// players can reclaim their entities.
struct SavedPlayer {
    std::string name;
    std::vector<EntityID> entityIds;
};

class GameServer {
public:
    bool Start(const ServerConfig& config);
    void Stop();
    void Update(float deltaTime);

    // Admin commands
    void KickPlayer(PlayerID id, const std::string& reason);
    void BroadcastSystemMessage(const std::string& message);
    void SaveWorld();
    void LoadWorld();
    void PrintStatus();
    void PrintPlayers();

    // Control de velocidad/pausa global (autoridad del servidor).
    // En MP la velocidad la dicta el SERVIDOR; los clientes la siguen vía TimeSync.
    // Estos métodos los invoca la consola del server (que solo tiene el host = admin).
    // Devuelven true si el cambio fue válido y se aplicó.
    bool SetGameSpeed(float speed);  // Fija velocidad global (rango 0.0-10.0)
    void PauseWorld();               // Pausa el mundo global (envía speed=0 a los clientes)
    void ResumeWorld();              // Reanuda el mundo a la velocidad configurada
    float GetGameSpeed() const { return m_config.gameSpeed; }
    bool  IsPaused() const { return m_serverPaused; }

    // ── Selector de facciones (autoridad del servidor) ──
    // Igual que velocidad/pausa: lo manda la consola del servidor (host = admin).
    // SetFactionMode: cambia el modo ("single"/"teams"/"per-player") en caliente y lo
    //   persiste en server.json. Reasigna las facciones de todos los jugadores conectados
    //   y se las notifica vía el paquete S2C_FactionAssignment. Devuelve false si el modo
    //   no es válido.
    bool SetFactionMode(const std::string& mode);
    // SetPlayerFaction: asigna manualmente al jugador 'id' la facción del slot indicado
    //   (slot 1-based, 1..N facciones del manifiesto). Notifica al cliente vía S2C.
    //   Devuelve false si el jugador no existe o el slot está fuera de rango.
    bool SetPlayerFaction(PlayerID id, int slot1Based);
    // PrintFactions: vuelca por consola las facciones del manifiesto y qué jugador tiene
    //   asignada cada una.
    void PrintFactions();
    // Guarda en disco la config actual (server.json). Lo usa SetFactionMode para persistir.
    void SaveConfig();
    // Ruta del fichero de config con el que se arrancó el server (para persistir cambios).
    void SetConfigPath(const std::string& path) { m_configPath = path; }

private:
    // Network
    void HandleConnect(ENetPeer* peer);
    void HandleDisconnect(ENetPeer* peer);
    void HandlePacket(ENetPeer* peer, const uint8_t* data, size_t size, int channel);

    // Message handlers
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

    // Broadcasting
    void Broadcast(const uint8_t* data, size_t len, int channel, uint32_t flags);
    void BroadcastExcept(PlayerID exclude, const uint8_t* data, size_t len, int channel, uint32_t flags);
    void SendTo(PlayerID id, const uint8_t* data, size_t len, int channel, uint32_t flags);

    // Game state
    void BroadcastPositions();
    void BroadcastTimeSync();
    void BroadcastHostAssignment();
    void BroadcastEntityHeartbeat();
    void SendWorldSnapshot(ConnectedPlayer& player);

    // Helpers
    ConnectedPlayer* GetPlayer(ENetPeer* peer);
    ConnectedPlayer* GetPlayer(PlayerID id);

    // Rate limiting: devuelve true si el jugador puede enviar OTRO mensaje del tipo
    // indicado en la ventana actual; false si ya superó el umbral (mensaje a descartar).
    // 'counter' es el contador del jugador a incrementar; 'maxPerSecond' su umbral.
    bool CheckRateLimit(ConnectedPlayer& player, uint32_t& counter, uint32_t maxPerSecond);

    // Comprueba si un jugador está baneado (por nombre o IP). Usado en handshake.
    bool IsBanned(const std::string& name, const ENetAddress& addr) const;
    PlayerID NextPlayerId();
    int GetReadyPlayerCount() const;
    int GetConnectedPlayerCount() const;

    // Query handler (responds without handshake for server browser)
    void HandleServerQuery(ENetPeer* peer, PacketReader& reader);

    // ── Helpers del selector de facciones ──
    // Carga el manifiesto faction-slots.json (lista ordenada de StringIds de facción de
    // jugador). Si no existe, cae a los 2 slots históricos (10-/12-) para no romper nada.
    void LoadFactionSlots();
    // Calcula el slot (0-based, acotado a m_factionSlots) que corresponde a un PlayerID
    // según el factionMode configurado (single/teams/per-player).
    int  ComputeFactionSlot(PlayerID id) const;
    // Envía el paquete S2C_FactionAssignment a un jugador con el slot indicado (0-based).
    // Centraliza el formato del paquete para que handshake y 'setfaction' usen lo mismo.
    // Actualiza player.factionSlot. Devuelve false si el slot está fuera de rango.
    bool SendFactionAssignment(ConnectedPlayer& player, int slot0Based);

    ENetHost* m_host = nullptr;
    ServerConfig m_config;
    std::string  m_configPath = "server.json"; // Ruta de config para persistir cambios en caliente

    // ── Slots de facción del manifiesto (faction-slots.json) ──
    // Lista ORDENADA de StringIds de facción de jugador ("10-kenshi-online.mod", ...).
    // El índice es el "slot" que se envía al cliente. Se carga en Start().
    std::vector<std::string> m_factionSlots;
    std::unordered_map<PlayerID, ConnectedPlayer> m_players;
    std::unordered_map<EntityID, ServerEntity> m_entities;
    std::unordered_map<std::string, SavedPlayer> m_savedPlayers; // name → saved entities (persisted)

    // ── Baneos (comando admin 'ban') ──
    // Conjuntos de nombres e IPs vetados. Se consultan en HandleHandshake para
    // impedir la reconexión de un jugador baneado. Persistidos en bans.json.
    std::unordered_set<std::string> m_bannedNames;
    std::unordered_set<std::string> m_bannedIPs;
    void LoadBans();   // Carga bans.json al arrancar
    void SaveBans();   // Persiste el estado de baneos a bans.json

    std::recursive_mutex m_mutex;

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

    // UPnP auto port mapping (runs synchronously before ENet host is created)
    UPnPMapper m_upnp;

    // Orphan entity cleanup
    float    m_timeSinceOrphanCleanup = 0.f;

    // Auto-save
    float    m_timeSinceAutoSave = 0.f;
    float    m_autoSaveInterval = 60.f; // seconds

    // Master server registration
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

// World persistence (defined in world_persistence.cpp)
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
