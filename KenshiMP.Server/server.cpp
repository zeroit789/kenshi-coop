// ES: server.cpp - Implementación de GameServer, el núcleo del servidor dedicado.
//     Contiene: arranque/parada (UPnP, host ENet), el tick Update() (bombeo de eventos ENet, reloj
//     del mundo, envíos periódicos, limpieza de huérfanos, autoguardado, master server), el
//     despacho de paquetes C2S con validación de canal, el handshake (versión, contraseña, baneos,
//     reconexión, elección de host, facción), todos los handlers de juego (posición, combate,
//     construcción, inventario, escuadras, puertas...), comandos de admin, selector de facciones,
//     baneos persistidos en bans.json y el registro opcional en el master server.
//     Regla de hilos: Update() toma m_mutex y los Handle* corren dentro; los métodos públicos
//     llamados desde la consola toman m_mutex por su cuenta (es recursivo).
// EN: server.cpp - GameServer implementation, the core of the dedicated server.
//     Contains: start/stop (UPnP, ENet host), the Update() tick (ENet event pump, world clock,
//     periodic broadcasts, orphan cleanup, autosave, master server), C2S packet dispatch with
//     channel validation, the handshake (version, password, bans, reconnect, host election,
//     faction), every gameplay handler (position, combat, building, inventory, squads, doors...),
//     admin commands, the faction selector, bans persisted in bans.json and the optional master
//     server registration.
//     Threading rule: Update() takes m_mutex and the Handle* functions run inside it; public
//     methods called from the console take m_mutex themselves (it is recursive).
#include "server.h"
#include "authority_validator.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>  // strnlen — usado para acotar lectura de buffers de texto sin null-terminar
#include <thread>

namespace kmp {

// ES: Utilidades internas de este fichero (enlace interno).
// EN: File-local helpers (internal linkage).
namespace {
// ES: true si la dirección ENet es 127.0.0.1 (en orden de red = 0x0100007F).
//     Sirve para detectar al cliente del host integrado: el injector lanza server.exe y se conecta
//     desde loopback, así que ese peer manda aunque un jugador de LAN ganara la carrera de conexión.
// EN: True when this ENet address is 127.0.0.1 (network byte order = 0x0100007F).
// Used to detect the integrated-host client: the injector launches server.exe
// and connects from loopback, so this peer is authoritative even if a LAN
// player briefly won the first-connect race.
bool IsLoopbackAddress(const ENetAddress& addr) {
    return addr.host == 0x0100007Fu;
}

// ES: Devuelve la IP de un peer como string (sin puerto). Usado para baneos por IP.
// EN: Returns a peer IP as a string (without port). Used for IP bans.
std::string AddressToIPString(const ENetAddress& addr) {
    char buf[64] = {0};
    if (enet_address_get_host_ip(&addr, buf, sizeof(buf)) == 0) {
        return std::string(buf);
    }
    return std::string();
}

// ES: ── Umbrales de rate limiting (mensajes por segundo y por jugador) ──
// Generosos para no afectar al juego legítimo; solo cortan flood evidente.
// EN: ── Rate limiting thresholds (messages per second, per player) ──
//     Generous so legitimate play is unaffected; they only cut obvious flooding.
//     Chat: 5/s; position updates: 40/s (tickRate 20 + margin); max chat length: 256.
constexpr uint32_t KMP_MAX_CHAT_PER_SEC = 5;    // 5 mensajes de chat/seg
constexpr uint32_t KMP_MAX_POS_PER_SEC  = 40;   // 40 position updates/seg (tickRate 20 + margen)
constexpr uint16_t KMP_MAX_CHAT_LEN     = 256;  // Longitud máx de chat (antes 1024 implícito)
} // namespace

// ES: Declaración adelantada copiada de combat_resolver.cpp (no hay cabecera compartida). La
//     estructura debe ser idéntica a la de allí o se rompe la ODR y los datos se leerían mal.
// EN: Forward declaration from combat_resolver.cpp (there is no shared header). The struct must
//     stay identical to the one there, otherwise the ODR is broken and data would be read wrongly.
struct CombatResult {
    BodyPart hitPart;
    float cutDamage;
    float bluntDamage;
    float pierceDamage;
    float resultHealth;
    bool wasBlocked;
    bool wasKO;
    bool wasDeath;
};
CombatResult ResolveCombat(const ServerEntity& attacker, ServerEntity& target, uint8_t attackType);

// ES: Arranca el servidor. Orden: ENet -> abrir puerto (UPnP o, si falla, regla de firewall de
//     Windows con netsh, que necesita admin) -> crear host ENet escuchando -> baneos -> facciones
//     -> master server. Devuelve false si ENet no inicializa o no se puede abrir el puerto UDP.
// EN: Starts the server. Order: ENet -> open the port (UPnP or, on failure, a Windows firewall rule
//     via netsh, which needs admin) -> create the listening ENet host -> bans -> factions ->
//     master server. Returns false if ENet fails to init or the UDP port cannot be bound.
bool GameServer::Start(const ServerConfig& config) {
    m_config = config;

    if (enet_initialize() != 0) {
        spdlog::error("GameServer: ENet initialization failed");
        return false;
    }

    // ES: ── UPnP / Firewall: ANTES de escuchar ──
    //     El servidor no acepta conexiones hasta que el puerto está mapeado.
    // EN: ── UPnP / Firewall: do this BEFORE listening ──
    // The server doesn't accept any connections until the port is mapped.
    spdlog::info("GameServer: Setting up port forwarding for port {}...", config.port);
    if (m_upnp.AddMapping(config.port, config.port, "UDP", "KenshiMP Server")) {
        std::string extIP = m_upnp.GetExternalIP();
        if (!extIP.empty()) {
            spdlog::info("GameServer: UPnP mapped! Others can join at {}:{}", extIP, config.port);
        } else {
            spdlog::info("GameServer: UPnP mapped port {} successfully", config.port);
        }
    } else {
        spdlog::info("GameServer: UPnP unavailable — adding Windows Firewall rule instead...");

        // ES: Plan B: borra la regla anterior (si la hay) y crea una regla de entrada UDP para el puerto.
        // EN: Fallback: delete any previous rule and add an inbound UDP rule for the port.
        std::string deleteCmd = "netsh advfirewall firewall delete rule name=\"KenshiMP Server\" >nul 2>&1";
        std::system(deleteCmd.c_str());

        std::string addCmd = "netsh advfirewall firewall add rule name=\"KenshiMP Server\" "
                             "dir=in action=allow protocol=UDP localport=" + std::to_string(config.port);
        int result = std::system(addCmd.c_str());
        if (result == 0) {
            spdlog::info("GameServer: Firewall rule added — port {} UDP is open", config.port);
        } else {
            spdlog::warn("GameServer: Failed to add firewall rule (need admin?). "
                         "Port {} may need manual forwarding.", config.port);
        }
    }

    // ES: ── Ahora a escuchar: el puerto ya está mapeado (o se ha intentado) ──
    // EN: ── Now start listening — port is mapped (or we tried our best) ──
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = config.port;

    // ES: Se reservan 4 veces más huecos de peer ENet que maxPlayers para absorber estados de transición
    //     en ciclos rápidos de conexión/desconexión. El límite lógico de jugadores lo impone
    //     m_players.size() en HandleConnect/HandleHandshake. Canales: KMP_CHANNEL_COUNT (3).
    // EN: Use extra ENet peer slots beyond maxPlayers to handle transitional
    // peer states during rapid connect/disconnect cycles. The logical player
    // limit is enforced by m_players.size() in HandleConnect/HandleHandshake.
    int peerSlots = config.maxPlayers * 4;
    m_host = enet_host_create(&address, peerSlots, KMP_CHANNEL_COUNT,
                              KMP_DOWNSTREAM_LIMIT, KMP_UPSTREAM_LIMIT);
    if (!m_host) {
        spdlog::error("GameServer: Failed to create ENet host on port {}", config.port);
        enet_deinitialize();
        return false;
    }

    spdlog::info("GameServer: Listening on port {} — ready for connections", config.port);

    // ES: Intervalo de autoguardado. OJO: el comentario dice "de la config" pero el valor está fijo a 60 s.
    // EN: Auto-save interval from config (default 60s)
    m_autoSaveInterval = 60.f;

    // ES: Cargar lista de baneados (bans.json) si existe
    // EN: Load the ban list (bans.json) if it exists.
    LoadBans();

    // ES: Cargar el manifiesto de slots de facción (faction-slots.json) generado por ModGen.
    // EN: Load the faction slot manifest (faction-slots.json) generated by ModGen.
    LoadFactionSlots();

    // ES: Conecta con el master server para aparecer en el navegador de servidores (opcional).
    // EN: Connect to master server for server browser registration
    ConnectToMaster();

    return true;
}

// ES: Apagado ordenado: desregistro del master, quitar el mapeo UPnP, desconectar a todos los
//     jugadores, vaciar la cola de ENet (hasta 1 s por evento) y destruir el host.
// EN: Orderly shutdown: master deregistration, remove the UPnP mapping, disconnect every player,
//     drain the ENet queue (up to 1 s per event) and destroy the host.
void GameServer::Stop() {
    // ES: H3: bloquear el mutex durante todo el apagado. El hilo de consola
    // (comandos status/players/etc.) también toma m_mutex y accede a m_players/m_host,
    // así que sin este lock un comando tecleado justo durante el shutdown leería
    // estructuras a medio destruir. m_mutex es recursive_mutex: las llamadas internas
    // que vuelven a bloquear (p.ej. SaveWorld/BroadcastSystemMessage) no causan deadlock.
    // EN: H3: hold the mutex for the whole shutdown. The console thread (status/players/etc. commands)
    //     also takes m_mutex and touches m_players/m_host, so without this lock a command typed right
    //     during shutdown would read half-destroyed structures. m_mutex is a recursive_mutex: internal
    //     calls that lock again (e.g. SaveWorld/BroadcastSystemMessage) do not deadlock.
    std::lock_guard lock(m_mutex);

    // ES: Darse de baja del master server y cerrar su host ENet aparte.
    // EN: Deregister from master server
    SendMasterDeregister();
    if (m_masterPeer) {
        enet_peer_disconnect_now(m_masterPeer, 0);
        m_masterPeer = nullptr;
    }
    if (m_masterHost) {
        enet_host_destroy(m_masterHost);
        m_masterHost = nullptr;
    }
    m_masterConnected = false;

    // ES: Quitar el mapeo de puerto UPnP del router.
    // EN: Remove UPnP port mapping
    if (m_upnp.IsMapped()) {
        m_upnp.RemoveMapping(m_config.port, "UDP");
    }

    // ES: Desconectar a todos los jugadores.
    // EN: Disconnect all players
    for (auto& [id, player] : m_players) {
        if (player.peer) {
            enet_peer_disconnect(player.peer, 0);
        }
    }

    // ES: Vaciar: procesa eventos pendientes para que salgan las desconexiones; descarta paquetes recibidos.
    // EN: Flush
    ENetEvent event;
    while (enet_host_service(m_host, &event, 1000) > 0) {
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        }
    }

    if (m_host) {
        enet_host_destroy(m_host);
        m_host = nullptr;
    }
    enet_deinitialize();
    m_players.clear();
}

// ES: Un tick del servidor (~20 Hz; deltaTime en segundos reales). Toma m_mutex durante todo el tick.
//     Pasos: bombear eventos ENet -> avanzar reloj del mundo -> posiciones (cada tick) -> TimeSync
//     (cada 5 s) -> heartbeat de entidades (cada 5 s) -> pings -> limpieza de huérfanos (cada 30 s)
//     -> autoguardado -> master server -> flush de ENet.
// EN: One server tick (~20 Hz; deltaTime in real seconds). Holds m_mutex for the whole tick.
//     Steps: pump ENet events -> advance world clock -> positions (every tick) -> TimeSync
//     (every 5 s) -> entity heartbeat (every 5 s) -> pings -> orphan cleanup (every 30 s)
//     -> autosave -> master server -> ENet flush.
void GameServer::Update(float deltaTime) {
    std::lock_guard lock(m_mutex);
    m_serverTick++;
    m_uptime += deltaTime;

    // ES: Bombea los eventos de ENet sin esperar (timeout 0): conexión, paquete recibido, desconexión.
    // EN: Pump ENet events
    ENetEvent event;
    while (enet_host_service(m_host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            HandleConnect(event.peer);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            HandlePacket(event.peer, event.packet->data, event.packet->dataLength,
                        event.channelID);
            enet_packet_destroy(event.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            HandleDisconnect(event.peer);
            break;
        default:
            break;
        }
    }

    // ES: Actualiza la hora del mundo: el reloj solo avanza si el mundo NO está pausado.
    //     Velocidad efectiva = 0 cuando está pausado (coherente con lo que se envía en TimeSync).
    //     Un día completo = 86400 s reales a velocidad 1.
    // EN: Update world time: the clock only advances while the world is NOT paused.
    //     Effective speed = 0 while paused (consistent with what TimeSync sends).
    //     One full day = 86400 real seconds at speed 1.
    float effectiveSpeed = m_serverPaused ? 0.f : m_config.gameSpeed;
    m_timeOfDay += deltaTime * effectiveSpeed / 86400.f; // 24h cycle
    if (m_timeOfDay >= 1.f) m_timeOfDay -= 1.f;

    // ES: Envía posiciones en cada tick.
    // EN: Broadcast positions every tick
    BroadcastPositions();

    // ES: Sincroniza hora/clima/velocidad cada 5 segundos.
    // EN: Time sync every 5 seconds
    m_timeSinceTimeSync += deltaTime;
    if (m_timeSinceTimeSync >= 5.0f) {
        BroadcastTimeSync();
        m_timeSinceTimeSync = 0.f;
    }

    // ES: Heartbeat de entidades cada 5 s: los clientes detectan y limpian entidades fantasma.
    // EN: Entity heartbeat every 5 seconds — clients detect and clean up ghost entities
    m_timeSinceHeartbeat += deltaTime;
    if (m_timeSinceHeartbeat >= 5.0f && !m_entities.empty()) {
        BroadcastEntityHeartbeat();
        m_timeSinceHeartbeat = 0.f;
    }

    // ES: Actualiza el ping de cada jugador con el RTT que mide ENet.
    // EN: Update player pings
    for (auto& [id, player] : m_players) {
        if (player.peer) {
            player.ping = player.peer->roundTripTime;
        }
    }

    // ES: Limpieza periódica de huérfanas: borra entidades cuyo dueño ya no está conectado (p. ej.
    //     restos de un guardado) y avisa a todos con S2C_EntityDespawn (motivo 2). Se filtran de los
    //     snapshots, pero se acumularían en m_entities si no se limpiaran.
    // EN: Periodic orphan entity cleanup — remove entities whose owner is no longer
    // connected (e.g., stale entries loaded from world save files). These are
    // filtered from snapshots but accumulate in m_entities if not cleaned.
    m_timeSinceOrphanCleanup += deltaTime;
    if (m_timeSinceOrphanCleanup >= 30.0f && !m_entities.empty()) {
        std::vector<EntityID> orphans;
        for (const auto& [eid, entity] : m_entities) {
            // ES: owner == 0 = entidad del servidor (del mundo): no se toca.
            // EN: owner == 0 means server-owned (world entity), skip those
            if (entity.owner != 0 && !GetPlayer(entity.owner)) {
                orphans.push_back(eid);
            }
        }
        if (!orphans.empty()) {
            for (EntityID eid : orphans) {
                // ES: Avisa a los jugadores que quedan para que quiten el fantasma.
                // EN: Broadcast despawn to all remaining players so they remove the ghost
                PacketWriter despawnWriter;
                despawnWriter.WriteHeader(MessageType::S2C_EntityDespawn);
                despawnWriter.WriteU32(eid);
                despawnWriter.WriteU8(2); // reason: orphan cleanup
                Broadcast(despawnWriter.Data(), despawnWriter.Size(),
                         KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
                m_entities.erase(eid);
            }
            spdlog::info("GameServer: Orphan cleanup removed {} entities with no connected owner",
                         orphans.size());
        }
        m_timeSinceOrphanCleanup = 0.f;
    }

    // ES: Autoguardado periódico (solo si hay entidades).
    // EN: Auto-save periodically
    m_timeSinceAutoSave += deltaTime;
    if (m_timeSinceAutoSave >= m_autoSaveInterval && !m_entities.empty()) {
        SaveWorld();
        m_timeSinceAutoSave = 0.f;
    }

    // ES: Heartbeat/reconexión con el master server.
    // EN: Master server heartbeat
    UpdateMasterConnection(deltaTime);

    // ES: Envía ya todos los paquetes en cola para que los clientes reciban las respuestas en este
    //     mismo tick y no en la siguiente llamada a enet_host_service.
    // EN: Flush all queued outgoing packets immediately so clients
    // receive responses within the same tick rather than waiting
    // for the next enet_host_service call.
    enet_host_flush(m_host);
}

// ES: Nueva conexión ENet (aún sin handshake). Rechaza si el servidor está lleno; si no, fija el
//     timeout (10 s mín / 15 s máx: un cliente colgado se detecta en ~15 s) y deja peer->data a
//     nullptr hasta que llegue un handshake válido.
// EN: New ENet connection (no handshake yet). Rejects if the server is full; otherwise sets the
//     timeout (10 s min / 15 s max: a crashed client is detected in ~15 s) and leaves peer->data
//     as nullptr until a valid handshake arrives.
void GameServer::HandleConnect(ENetPeer* peer) {
    char addrStr[64];
    enet_address_get_host_ip(&peer->address, addrStr, sizeof(addrStr));
    spdlog::info("GameServer: Incoming connection from {}:{}", addrStr, peer->address.port);

    if (m_players.size() >= static_cast<size_t>(m_config.maxPlayers)) {
        spdlog::warn("GameServer: Server full, rejecting connection");
        enet_peer_disconnect(peer, 0);
        return;
    }

    // ES: Conexión aceptada; se espera al handshake.
    // EN: Connection accepted, wait for handshake
    // 10s min / 15s max timeout — detect crashed clients within ~15 seconds
    enet_peer_timeout(peer, 0, 10000, 15000);
    peer->data = nullptr;
}

// ES: Desconexión de un peer (evento ENet, se llama exactamente una vez). Si era un jugador:
//     conserva sus entidades para que pueda reconectar (owner = 0 + registro en m_savedPlayers),
//     avisa con S2C_PlayerLeft, lo borra y, si era el host, pasa el host a otro jugador.
// EN: Peer disconnection (ENet event, called exactly once). If it was a player: keeps its entities
//     so it can reconnect (owner = 0 + record in m_savedPlayers), sends S2C_PlayerLeft, removes it
//     and, if it was the host, hands the host role to another player.
void GameServer::HandleDisconnect(ENetPeer* peer) {
    ConnectedPlayer* player = GetPlayer(peer);
    if (player) {
        spdlog::info("GameServer: Player '{}' (ID: {}) disconnected", player->name, player->id);

        // ES: Conserva las entidades para la reconexión: quedan sin dueño y se apunta nombre -> entidades.
        // EN: Preserve entities for reconnection — mark them as unowned and record
        // the player→entity mapping so the player can reclaim them later.
        std::vector<EntityID> ownedIds;
        for (auto& [eid, entity] : m_entities) {
            if (entity.owner == player->id) {
                entity.owner = 0; // Unowned until reconnect
                ownedIds.push_back(eid);
            }
        }
        if (!ownedIds.empty()) {
            SavedPlayer sp;
            sp.name = player->name;
            sp.entityIds = ownedIds;
            m_savedPlayers[player->name] = std::move(sp);
            spdlog::info("GameServer: Preserved {} entities for player '{}' (reconnectable)",
                         ownedIds.size(), player->name);
        }

        // ES: Avisa a los demás de que el jugador se ha ido.
        // EN: Notify others that player left
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_PlayerLeft);
        MsgPlayerLeft msg;
        msg.playerId = player->id;
        msg.reason = 0; // disconnect
        writer.WriteRaw(&msg, sizeof(msg));
        BroadcastExcept(player->id, writer.Data(), writer.Size(),
                       KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);

        // ES: Borrar al jugador PRIMERO para que la reasignación de host no lo tenga en cuenta.
        // EN: Erase player FIRST so host reassignment loop doesn't see them
        PlayerID leavingId = player->id;
        std::string leavingName = player->name;
        m_players.erase(leavingId);

        // ES: Si se fue el host, el nuevo host es el primer jugador del mapa (orden no definido de
        //     unordered_map, no necesariamente el más antiguo) y se avisa a todos.
        // EN: Reassign host if the host disconnected
        if (leavingId == m_hostPlayerId) {
            m_hostPlayerId = 0;
            if (!m_players.empty()) {
                auto it = m_players.begin();
                m_hostPlayerId = it->first;
                spdlog::info("GameServer: Host reassigned to '{}' (ID: {})", it->second.name, it->first);
                BroadcastSystemMessage(it->second.name + " is now the host");
                BroadcastHostAssignment();  // notify all clients of the new host id
            } else {
                spdlog::info("GameServer: Last player left — no host assigned");
            }
        }

        // ES: Mensaje de sistema "X left the game".
        // EN: Broadcast system message
        BroadcastSystemMessage(leavingName + " left the game");
    }
    peer->data = nullptr;
}

// ES: Punto de entrada de todo paquete recibido. Valida el canal y despacha por MessageType al
//     handler. Los mensajes de juego exigen un jugador con handshake (GetPlayer(peer) != nullptr);
//     Handshake y ServerQuery no. Corre dentro de Update() con m_mutex tomado.
// EN: Entry point for every received packet. Validates the channel and dispatches by MessageType to
//     the handler. Gameplay messages require a handshaken player (GetPlayer(peer) != nullptr);
//     Handshake and ServerQuery do not. Runs inside Update() with m_mutex held.
void GameServer::HandlePacket(ENetPeer* peer, const uint8_t* data, size_t size, int channel) {
    if (size < sizeof(PacketHeader)) return;

    // ES: Nota: m_mutex ya lo tiene Update(); es recursive_mutex para que los métodos públicos (KickPlayer, etc.) también puedan bloquear.
    // EN: Note: m_mutex is already held by Update() which calls this method.
    // Using recursive_mutex so public methods (KickPlayer, etc.) can also lock safely.

    // ES: Validación de canal: las posiciones solo por el canal no fiable (2); el resto de mensajes de
    //     juego solo por canales fiables (0 ordenado / 1 no ordenado), salvo el keepalive.
    // EN: Channel validation: reject messages arriving on the wrong channel.
    // Position updates must use the unreliable channel; all other gameplay
    // messages must arrive on a reliable channel (ordered or unordered).
    PacketReader peekReader(data, size);
    PacketHeader peekHeader;
    if (peekReader.ReadHeader(peekHeader)) {
        bool isPositionUpdate = (peekHeader.type == MessageType::C2S_PositionUpdate);
        if (isPositionUpdate && channel != KMP_CHANNEL_UNRELIABLE_SEQ) {
            spdlog::debug("GameServer: Position update on wrong channel {} (expected {})",
                         channel, KMP_CHANNEL_UNRELIABLE_SEQ);
            return;
        }
        if (!isPositionUpdate && channel == KMP_CHANNEL_UNRELIABLE_SEQ &&
            peekHeader.type != MessageType::C2S_Keepalive) {
            spdlog::debug("GameServer: Reliable message 0x{:02X} on unreliable channel",
                         static_cast<uint8_t>(peekHeader.type));
            return;
        }
    }

    PacketReader reader(data, size);
    PacketHeader header;
    if (!reader.ReadHeader(header)) return;

    // ES: Despacho por tipo de mensaje C2S. El patrón es siempre: buscar el jugador del peer y, si
    //     existe (handshake hecho), llamar a su handler.
    // EN: Dispatch by C2S message type. The pattern is always: look up the peer's player and, if it
    //     exists (handshake done), call its handler.
    switch (header.type) {
    case MessageType::C2S_Handshake:
        HandleHandshake(peer, reader);
        break;
    case MessageType::C2S_PositionUpdate: {
        auto* player = GetPlayer(peer);
        if (player) HandlePositionUpdate(*player, reader);
        break;
    }
    case MessageType::C2S_MoveCommand: {
        auto* player = GetPlayer(peer);
        if (player) HandleMoveCommand(*player, reader);
        break;
    }
    case MessageType::C2S_AttackIntent: {
        auto* player = GetPlayer(peer);
        if (player) HandleAttackIntent(*player, reader);
        break;
    }
    case MessageType::C2S_ChatMessage: {
        auto* player = GetPlayer(peer);
        if (player) HandleChatMessage(*player, reader);
        break;
    }
    case MessageType::C2S_BuildRequest: {
        auto* player = GetPlayer(peer);
        if (player) HandleBuildRequest(*player, reader);
        break;
    }
    case MessageType::C2S_EntitySpawnReq: {
        auto* player = GetPlayer(peer);
        if (player) HandleEntitySpawnReq(*player, reader);
        break;
    }
    case MessageType::C2S_EntityDespawnReq: {
        auto* player = GetPlayer(peer);
        if (player) HandleEntityDespawnReq(*player, reader);
        break;
    }
    case MessageType::C2S_EquipmentUpdate: {
        auto* player = GetPlayer(peer);
        if (player) HandleEquipmentUpdate(*player, reader);
        break;
    }
    case MessageType::C2S_ZoneRequest: {
        auto* player = GetPlayer(peer);
        if (player) HandleZoneRequest(*player, reader);
        break;
    }
    case MessageType::C2S_ItemPickup: {
        auto* player = GetPlayer(peer);
        if (player) HandleItemPickup(*player, reader);
        break;
    }
    case MessageType::C2S_ItemDrop: {
        auto* player = GetPlayer(peer);
        if (player) HandleItemDrop(*player, reader);
        break;
    }
    case MessageType::C2S_TradeRequest: {
        auto* player = GetPlayer(peer);
        if (player) HandleTradeRequest(*player, reader);
        break;
    }
    case MessageType::C2S_SquadCreate: {
        auto* player = GetPlayer(peer);
        if (player) HandleSquadCreate(*player, reader);
        break;
    }
    case MessageType::C2S_SquadAddMember: {
        auto* player = GetPlayer(peer);
        if (player) HandleSquadAddMember(*player, reader);
        break;
    }
    case MessageType::C2S_FactionRelation: {
        auto* player = GetPlayer(peer);
        if (player) HandleFactionRelation(*player, reader);
        break;
    }
    case MessageType::C2S_PlayerReady: {
        auto* player = GetPlayer(peer);
        if (player) HandlePlayerReady(*player);
        break;
    }
    case MessageType::C2S_BuildDismantle: {
        auto* player = GetPlayer(peer);
        if (player) HandleBuildDismantle(*player, reader);
        break;
    }
    case MessageType::C2S_BuildRepair: {
        auto* player = GetPlayer(peer);
        if (player) HandleBuildRepair(*player, reader);
        break;
    }
    case MessageType::C2S_CombatStance: {
        auto* player = GetPlayer(peer);
        if (player) HandleCombatStance(*player, reader);
        break;
    }
    case MessageType::C2S_CombatKO: {
        auto* player = GetPlayer(peer);
        if (player) HandleCombatKO(*player, reader);
        break;
    }
    case MessageType::C2S_CombatDeath: {
        auto* player = GetPlayer(peer);
        if (player) HandleCombatDeath(*player, reader);
        break;
    }
    case MessageType::C2S_LimbHealth: {
        auto* player = GetPlayer(peer);
        if (player) HandleLimbHealth(*player, reader);
        break;
    }
    case MessageType::C2S_StatusEffect: {
        auto* player = GetPlayer(peer);
        if (player) HandleStatusEffect(*player, reader);
        break;
    }
    case MessageType::C2S_ItemTransfer: {
        auto* player = GetPlayer(peer);
        if (player) HandleItemTransfer(*player, reader);
        break;
    }
    case MessageType::C2S_DoorInteract: {
        auto* player = GetPlayer(peer);
        if (player) HandleDoorInteract(*player, reader);
        break;
    }
    case MessageType::C2S_AdminCommand: {
        auto* player = GetPlayer(peer);
        if (player) HandleAdminCommand(*player, reader);
        break;
    }
    case MessageType::C2S_LobbyReady: {
        auto* player = GetPlayer(peer);
        if (player) HandleLobbyReady(*player, reader);
        break;
    }
    case MessageType::C2S_Keepalive: {
        // ES: Keepalive: reinicia el temporizador de actividad y responde con S2C_KeepaliveAck.
        // EN: Reset activity timer and send ack
        auto* player = GetPlayer(peer);
        if (player) {
            player->lastUpdate = m_uptime;
            PacketWriter ackWriter;
            ackWriter.WriteHeader(MessageType::S2C_KeepaliveAck);
            ENetPacket* pkt = enet_packet_create(ackWriter.Data(), ackWriter.Size(), ENET_PACKET_FLAG_RELIABLE);
            if (!pkt) {
                spdlog::error("Failed to create packet ({} bytes)", ackWriter.Size());
                break;
            }
            enet_peer_send(peer, KMP_CHANNEL_RELIABLE_ORDERED, pkt);
        }
        break;
    }
    case MessageType::C2S_Disconnect: {
        // ES: Desconexión limpia pedida por el cliente. No se llama a HandleDisconnect aquí: solo se pide a
        //     ENet desconectar; el evento DISCONNECT llamará a HandleDisconnect una sola vez (evita doble
        //     limpieza y carreras de reutilización del hueco del peer).
        // EN: Graceful disconnect requested by client.
        // Don't call HandleDisconnect here — just tell ENet to disconnect.
        // The ENET_EVENT_TYPE_DISCONNECT event will fire and call HandleDisconnect
        // exactly once, preventing double-cleanup and peer slot reuse races.
        auto* player = GetPlayer(peer);
        if (player) {
            spdlog::info("GameServer: Player '{}' sent graceful disconnect", player->name);
        }
        enet_peer_disconnect(peer, 0);
        break;
    }
    case MessageType::C2S_ServerQuery:
        HandleServerQuery(peer, reader);
        break;
    case MessageType::C2S_PipelineSnapshot:
    case MessageType::C2S_PipelineEvent: {
        // ES: Depuración del pipeline: reenvío puro; el servidor no lo interpreta, solo lo manda al resto añadiendo el ID del emisor.
        // EN: Pipeline debug: pure relay — server doesn't interpret, just forwards to all other clients
        auto* player = GetPlayer(peer);
        if (player) {
            MessageType fwdType = (header.type == MessageType::C2S_PipelineSnapshot)
                ? MessageType::S2C_PipelineSnapshot : MessageType::S2C_PipelineEvent;
            PacketWriter fwd;
            fwd.WriteHeader(fwdType);
            fwd.WriteU32(player->id);  // Prepend sender player ID
            fwd.WriteRaw(reader.Current(), reader.Remaining());
            BroadcastExcept(player->id, fwd.Data(), fwd.Size(),
                            KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
        }
        break;
    }
    default:
        spdlog::debug("GameServer: Unknown message type 0x{:02X}", static_cast<uint8_t>(header.type));
        break;
    }
}

// ES: ── Rate limiting ──
// Cuenta mensajes en ventanas de 1 segundo. El host loopback (admin físico) queda exento.
// Devuelve true si el mensaje se acepta, false si excede el umbral y debe descartarse.
// EN: ── Rate limiting ──
//     Counts messages in 1-second windows. The loopback host (physical admin) is exempt.
//     Returns true if the message is accepted, false if it exceeds the threshold and must be dropped.
bool GameServer::CheckRateLimit(ConnectedPlayer& player, uint32_t& counter, uint32_t maxPerSecond) {
    // ES: El host integrado (loopback) no se limita: es el anfitrión, no un atacante remoto.
    // EN: The integrated host (loopback) is not limited: it is the host, not a remote attacker.
    if (player.isLoopback) return true;

    // ES: Reinicia la ventana si ha pasado 1 segundo desde su inicio.
    // EN: Reset the window if 1 second has passed since it started (resets both counters).
    if (m_uptime - player.rateWindowStart >= 1.0f) {
        player.rateWindowStart = m_uptime;
        player.chatMsgCount = 0;
        player.posMsgCount  = 0;
    }
    if (counter >= maxPerSecond) {
        return false; // Excedido — descartar
    }
    counter++;
    return true;
}

// ES: ── Baneos ──
// EN: ── Bans ──
//     IsBanned: true if the name or the peer IP is on the list.
bool GameServer::IsBanned(const std::string& name, const ENetAddress& addr) const {
    if (m_bannedNames.count(name) > 0) return true;
    std::string ip = AddressToIPString(addr);
    if (!ip.empty() && m_bannedIPs.count(ip) > 0) return true;
    return false;
}

// ES: Carga bans.json ({"names": [...], "ips": [...]}) del directorio de trabajo; si está corrupto se ignora.
// EN: Loads bans.json ({"names": [...], "ips": [...]}) from the working directory; ignored if corrupt.
void GameServer::LoadBans() {
    std::ifstream file("bans.json");
    if (!file.is_open()) return; // Sin baneos previos, normal
    try {
        nlohmann::json j;
        file >> j;
        if (j.contains("names"))
            m_bannedNames = j["names"].get<std::unordered_set<std::string>>();
        if (j.contains("ips"))
            m_bannedIPs = j["ips"].get<std::unordered_set<std::string>>();
        spdlog::info("GameServer: Cargados {} baneos por nombre, {} por IP",
                     m_bannedNames.size(), m_bannedIPs.size());
    } catch (...) {
        spdlog::warn("GameServer: bans.json corrupto, ignorado");
    }
}

// ES: Escribe los baneos actuales en bans.json (con sangría de 2 espacios).
// EN: Writes the current bans to bans.json (2-space indentation).
void GameServer::SaveBans() {
    nlohmann::json j;
    j["names"] = m_bannedNames;
    j["ips"]   = m_bannedIPs;
    std::ofstream file("bans.json");
    if (!file.is_open()) {
        spdlog::warn("GameServer: No se pudo escribir bans.json");
        return;
    }
    file << j.dump(2);
}

// ──────────────────────────────────────────────────────────────────────────
// ES: Selector de facciones — implementación
// ──────────────────────────────────────────────────────────────────────────
// EN: Faction selector - implementation

void GameServer::LoadFactionSlots() {
    // ES: Carga el manifiesto faction-slots.json generado por ModGen junto al .mod.
    // Estructura: {"factionSlots": ["10-kenshi-online.mod", "12-...", ...]}.
    // El índice de cada string es el "slot" que se envía al cliente.
    // EN: Loads the faction-slots.json manifest generated by ModGen next to the .mod.
    //     Layout: {"factionSlots": ["10-kenshi-online.mod", "12-...", ...]}.
    //     Each string's index is the "slot" sent to the client.
    m_factionSlots.clear();
    std::ifstream file("faction-slots.json");
    if (file.is_open()) {
        try {
            nlohmann::json j;
            file >> j;
            if (j.contains("factionSlots") && j["factionSlots"].is_array()) {
                for (const auto& s : j["factionSlots"]) {
                    if (s.is_string()) m_factionSlots.push_back(s.get<std::string>());
                }
            }
        } catch (...) {
            spdlog::warn("GameServer: faction-slots.json corrupto, usando slots por defecto");
        }
    }

    // ES: Fallback: si no hay manifiesto (o vino vacío), usar los 2 slots históricos.
    // Garantiza que el co-op básico sigue funcionando aunque falte el fichero.
    // EN: Fallback: with no manifest (or an empty one), use the 2 historical slots.
    //     Guarantees basic co-op keeps working even if the file is missing.
    if (m_factionSlots.empty()) {
        m_factionSlots = { "10-kenshi-online.mod", "12-kenshi-online.mod" };
        spdlog::info("GameServer: faction-slots.json no encontrado — usando 2 facciones por defecto (10-/12-)");
    } else {
        spdlog::info("GameServer: Cargadas {} facciones de jugador del manifiesto", m_factionSlots.size());
    }
}

int GameServer::ComputeFactionSlot(PlayerID id) const {
    // ES: Calcula el slot 0-based según factionMode:
    //   "single"     → todos slot 0 → todos comparten la facción del Player 1 (co-op puro).
    //   "teams"      → grupos de teamSize comparten facción: slot = (id-1)/teamSize.
    //   "per-player" → cada jugador su facción: slot = id-1.
    // En todos los casos se acota al nº real de facciones del manifiesto; si se agotan,
    // se cae a la última disponible para no enviar slots inexistentes (per-player → co-op).
    // EN: Computes the 0-based slot according to factionMode:
    //       "single"     -> everyone slot 0 -> everyone shares Player 1's faction (pure co-op).
    //       "teams"      -> groups of teamSize share a faction: slot = (id-1)/teamSize.
    //       "per-player" -> each player its own faction: slot = id-1.
    //     It is always clamped to the real number of manifest factions; once they run out, the last
    //     one is used so no nonexistent slot is sent (per-player degrades to co-op).
    int n = static_cast<int>(m_factionSlots.size());
    if (n <= 0) return 0;

    int slot;
    if (m_config.factionMode == "single") {
        slot = 0;
    } else if (m_config.factionMode == "teams") {
        int ts = m_config.teamSize > 0 ? m_config.teamSize : 1;
        slot = (static_cast<int>(id) - 1) / ts;
    } else { // "per-player"
        slot = static_cast<int>(id) - 1;
    }

    if (slot < 0) slot = 0;
    // ES: Si nos pasamos del nº de facciones, acotamos a la última (degrada a co-op en ese slot).
    // EN: If we run past the faction count, clamp to the last one (degrades to co-op in that slot).
    if (slot >= n) slot = n - 1;
    return slot;
}

bool GameServer::SendFactionAssignment(ConnectedPlayer& player, int slot0Based) {
    // ES: Centraliza el envío del paquete S2C_FactionAssignment (usado por handshake y setfaction).
    // Formato: [header][u16 len][raw factionString][i32 slot]. El cliente parchea ese string
    // en .rdata para controlar la facción correspondiente.
    // EN: Centralizes sending S2C_FactionAssignment (used by the handshake and setfaction).
    //     Format: [header][u16 len][raw factionString][i32 slot]. The client patches that string
    //     into .rdata to control the matching faction.
    if (slot0Based < 0 || slot0Based >= static_cast<int>(m_factionSlots.size())) {
        spdlog::warn("GameServer: slot de facción {} fuera de rango (hay {} facciones)",
                     slot0Based, m_factionSlots.size());
        return false;
    }
    const std::string& factionStr = m_factionSlots[slot0Based];

    PacketWriter factionWriter;
    factionWriter.WriteHeader(MessageType::S2C_FactionAssignment);
    factionWriter.WriteU16(static_cast<uint16_t>(factionStr.size()));
    factionWriter.WriteRaw(factionStr.data(), factionStr.size());
    factionWriter.WriteI32(slot0Based);

    ENetPacket* factionPkt = enet_packet_create(factionWriter.Data(), factionWriter.Size(),
                                                 ENET_PACKET_FLAG_RELIABLE);
    if (!factionPkt) {
        spdlog::error("GameServer: fallo al crear paquete de facción ({} bytes)", factionWriter.Size());
        return false;
    }
    if (player.peer) {
        enet_peer_send(player.peer, KMP_CHANNEL_RELIABLE_ORDERED, factionPkt);
    } else {
        enet_packet_destroy(factionPkt);
        return false;
    }

    player.factionSlot = slot0Based;
    spdlog::info("GameServer: Asignada facción '{}' (slot {}) al jugador {} ('{}')",
                 factionStr, slot0Based, player.id, player.name);
    return true;
}

bool GameServer::SetFactionMode(const std::string& mode) {
    // ES: Cambia el modo de facciones en caliente, lo persiste y reasigna a todos los conectados.
    // EN: Changes the faction mode at runtime, persists it and reassigns every connected player.
    if (mode != "single" && mode != "teams" && mode != "per-player") {
        return false;
    }
    std::lock_guard lock(m_mutex);
    m_config.factionMode = mode;
    SaveConfig(); // Persistir en server.json

    // ES: Reasignar facción a cada jugador conectado según el nuevo modo y notificarle.
    // EN: Reassign each connected player a faction for the new mode and notify them.
    for (auto& [id, player] : m_players) {
        int slot = ComputeFactionSlot(id);
        SendFactionAssignment(player, slot);
    }
    spdlog::info("GameServer: factionMode cambiado a '{}' — {} jugadores reasignados",
                 mode, m_players.size());
    BroadcastSystemMessage("El servidor cambió el modo de facciones a: " + mode);
    return true;
}

bool GameServer::SetPlayerFaction(PlayerID id, int slot1Based) {
    // ES: Asignación manual: el admin fija la facción de un jugador concreto (slot 1-based).
    // EN: Manual assignment: the admin sets one specific player faction (1-based slot).
    std::lock_guard lock(m_mutex);
    ConnectedPlayer* player = GetPlayer(id);
    if (!player) {
        spdlog::warn("GameServer: setfaction — jugador {} no encontrado", id);
        return false;
    }
    int slot0 = slot1Based - 1; // Convertir a 0-based (interno)
    if (slot0 < 0 || slot0 >= static_cast<int>(m_factionSlots.size())) {
        spdlog::warn("GameServer: setfaction — slot {} fuera de rango (1..{})",
                     slot1Based, m_factionSlots.size());
        return false;
    }
    return SendFactionAssignment(*player, slot0);
}

void GameServer::PrintFactions() {
    // ES: Lista las facciones del manifiesto y qué jugador tiene asignada cada una.
    // EN: Lists the manifest factions and which player holds each one.
    std::lock_guard lock(m_mutex);
    spdlog::info("=== Facciones (modo: {}, teamSize: {}) ===", m_config.factionMode, m_config.teamSize);
    if (m_factionSlots.empty()) {
        spdlog::info("  (sin facciones cargadas)");
        return;
    }
    for (size_t i = 0; i < m_factionSlots.size(); ++i) {
        // ES: Buscar qué jugadores tienen este slot asignado.
        // EN: Find which players have this slot assigned.
        std::string owners;
        for (auto& [id, player] : m_players) {
            if (player.factionSlot == static_cast<int>(i)) {
                if (!owners.empty()) owners += ", ";
                owners += "[" + std::to_string(id) + "] " + player.name;
            }
        }
        spdlog::info("  slot {} (Player {}): {}  ->  {}",
                     i + 1, i + 1, m_factionSlots[i],
                     owners.empty() ? "(libre)" : owners);
    }
}

void GameServer::SaveConfig() {
    // ES: Persiste la config actual en el fichero con el que se arrancó (server.json por defecto).
    // EN: Persists the current config to the file the server started with (server.json by default).
    if (m_config.Save(m_configPath)) {
        spdlog::info("GameServer: config guardada en {}", m_configPath);
    } else {
        spdlog::warn("GameServer: no se pudo escribir la config en {}", m_configPath);
    }
}

// ES: Handshake (C2S_Handshake): convierte un peer conectado en jugador. Validaciones en orden:
//     handshake duplicado, servidor lleno (reject 0), versión de protocolo (reject 1), nombre
//     saneado, contraseña opcional (reject 3), baneo (reject 2). Después: crea el jugador, reclama
//     entidades si reconecta con el mismo nombre, elige host (loopback gana), y envía en este orden
//     HandshakeAck -> HostAssignment (si cambió) -> FactionAssignment -> PlayerJoined a los demás ->
//     lista de jugadores existentes al nuevo -> snapshot del mundo -> mensaje "joined".
// EN: Handshake (C2S_Handshake): turns a connected peer into a player. Checks in order: duplicate
//     handshake, server full (reject 0), protocol version (reject 1), sanitized name, optional
//     password (reject 3), ban (reject 2). Then: creates the player, reclaims entities when it
//     reconnects with the same name, elects the host (loopback wins) and sends, in order,
//     HandshakeAck -> HostAssignment (if changed) -> FactionAssignment -> PlayerJoined to the others
//     -> existing player list to the newcomer -> world snapshot -> "joined" message.
void GameServer::HandleHandshake(ENetPeer* peer, PacketReader& reader) {
    MsgHandshake msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: Handshake duplicado: si el peer ya tiene jugador, se ignora.
    // EN: Duplicate connection check: reject if this peer already has a player
    if (peer->data != nullptr) {
        uintptr_t existingId = reinterpret_cast<uintptr_t>(peer->data);
        spdlog::warn("GameServer: Peer already has player ID {} — rejecting duplicate handshake", existingId);
        return;
    }

    // ES: Servidor lleno: reject con código 0 y desconexión diferida (deja salir el paquete).
    // EN: Check if server is full
    if (m_players.size() >= static_cast<size_t>(m_config.maxPlayers)) {
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_HandshakeReject);
        MsgHandshakeReject reject{};
        reject.reasonCode = 0; // full
        snprintf(reject.reasonText, sizeof(reject.reasonText), "Server is full (%d/%d)",
                 (int)m_players.size(), m_config.maxPlayers);
        writer.WriteRaw(&reject, sizeof(reject));
        ENetPacket* pkt = enet_packet_create(writer.Data(), writer.Size(), ENET_PACKET_FLAG_RELIABLE);
        if (!pkt) {
            spdlog::error("Failed to create packet ({} bytes)", writer.Size());
            enet_peer_disconnect_later(peer, 0);
            return;
        }
        enet_peer_send(peer, KMP_CHANNEL_RELIABLE_ORDERED, pkt);
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    // ES: Versión de protocolo distinta: reject con código 1.
    // EN: Verify protocol version
    if (msg.protocolVersion != KMP_PROTOCOL_VERSION) {
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_HandshakeReject);
        MsgHandshakeReject reject{};
        reject.reasonCode = 1;
        snprintf(reject.reasonText, sizeof(reject.reasonText),
                "Version mismatch: server=%d, client=%d", KMP_PROTOCOL_VERSION, msg.protocolVersion);
        writer.WriteRaw(&reject, sizeof(reject));

        ENetPacket* pkt = enet_packet_create(writer.Data(), writer.Size(), ENET_PACKET_FLAG_RELIABLE);
        if (!pkt) {
            spdlog::error("Failed to create packet ({} bytes)", writer.Size());
            enet_peer_disconnect_later(peer, 0);
            return;
        }
        enet_peer_send(peer, KMP_CHANNEL_RELIABLE_ORDERED, pkt);
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    // ES: Sanea el nombre: solo ASCII imprimible (32-126) y como mucho KMP_MAX_NAME_LENGTH; vacío -> "Player".
    // EN: Sanitize player name: strip non-printable ASCII, enforce length
    std::string sanitizedName;
    for (int i = 0; i < KMP_MAX_NAME_LENGTH && msg.playerName[i] != '\0'; i++) {
        char c = msg.playerName[i];
        if (c >= 32 && c < 127) sanitizedName += c;
    }
    if (sanitizedName.empty()) sanitizedName = "Player";

    // ES: ── Password opcional (lectura tolerante a tamaño) ──
    // Si quedan bytes tras la base del handshake, el cliente envió una contraseña
    // (U16 len + string). Clientes antiguos no la mandan → 'clientPassword' queda vacía.
    // EN: ── Optional password (size-tolerant read) ──
    //     If bytes remain after the base handshake, the client sent a password (U16 len + string).
    //     Old clients do not send it -> 'clientPassword' stays empty.
    std::string clientPassword;
    if (reader.Remaining() >= sizeof(uint16_t)) {
        reader.ReadString(clientPassword, KMP_MAX_PASSWORD_LENGTH);
    }

    // ES: Si el server tiene contraseña configurada, debe coincidir.
    // EN: If the server has a password configured, it must match (reject code 3).
    if (!m_config.password.empty() && clientPassword != m_config.password) {
        spdlog::warn("GameServer: Rechazado '{}' por contrasena incorrecta", sanitizedName);
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_HandshakeReject);
        MsgHandshakeReject reject{};
        reject.reasonCode = 3; // other
        snprintf(reject.reasonText, sizeof(reject.reasonText), "Contrasena incorrecta");
        writer.WriteRaw(&reject, sizeof(reject));
        ENetPacket* pkt = enet_packet_create(writer.Data(), writer.Size(), ENET_PACKET_FLAG_RELIABLE);
        if (pkt) enet_peer_send(peer, KMP_CHANNEL_RELIABLE_ORDERED, pkt);
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    // ES: ── Comprobación de baneo (por nombre o IP) ──
    // EN: ── Ban check (by name or IP) -> reject code 2 ──
    if (IsBanned(sanitizedName, peer->address)) {
        spdlog::warn("GameServer: Rechazada conexion de jugador baneado '{}'", sanitizedName);
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_HandshakeReject);
        MsgHandshakeReject reject{};
        reject.reasonCode = 2; // banned/kicked
        snprintf(reject.reasonText, sizeof(reject.reasonText), "Estas baneado de este servidor");
        writer.WriteRaw(&reject, sizeof(reject));
        ENetPacket* pkt = enet_packet_create(writer.Data(), writer.Size(), ENET_PACKET_FLAG_RELIABLE);
        if (pkt) enet_peer_send(peer, KMP_CHANNEL_RELIABLE_ORDERED, pkt);
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    // ES: Crea el jugador. peer->data guarda su PlayerID como entero (no es un puntero real);
    //     GetPlayer(peer) lo convierte de vuelta.
    // EN: Create player
    PlayerID id = NextPlayerId();
    ConnectedPlayer player;
    player.id = id;
    player.name = sanitizedName;
    player.peer = peer;
    player.ping = peer->roundTripTime;
    player.lastUpdate = m_uptime;

    player.isLoopback = IsLoopbackAddress(peer->address);
    if (player.isLoopback) {
        spdlog::info("GameServer: Player '{}' (ID: {}) connected from loopback — integrated host candidate",
                     player.name, id);
    }

    peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
    m_players[id] = player;

    // ES: Reconexión: si el nombre coincide con un registro guardado, recupera las entidades que sigan sin dueño.
    // EN: Reconnect: if this player name matches a saved record, reclaim their entities
    auto savedIt = m_savedPlayers.find(sanitizedName);
    if (savedIt != m_savedPlayers.end()) {
        int reclaimed = 0;
        for (EntityID eid : savedIt->second.entityIds) {
            auto entIt = m_entities.find(eid);
            if (entIt != m_entities.end() && entIt->second.owner == 0) {
                entIt->second.owner = id;
                m_players[id].ownedEntities.push_back(eid);
                reclaimed++;
            }
        }
        spdlog::info("GameServer: Player '{}' reconnected — reclaimed {}/{} entities",
                     sanitizedName, reclaimed, savedIt->second.entityIds.size());
        m_savedPlayers.erase(savedIt);
    }

    // ES: Elección de host: el peer loopback siempre gana al que no lo es.
    //     (a) Aún no hay host -> este jugador es host, sea loopback o no.
    //     (b) El host actual no es loopback Y este sí -> lo sustituye.
    //     (c) El host actual es loopback O este no lo es -> se mantiene el actual.
    //     El host es también el admin (solo él puede usar C2S_AdminCommand).
    // EN: Host assignment: loopback peer always wins over non-loopback.
    // Scenarios:
    //   (a) No host yet → this player becomes host regardless of loopback.
    //   (b) Current host is non-loopback AND this player is loopback → override.
    //   (c) Current host is loopback OR this player is non-loopback → keep current.
    bool hostChanged = false;
    if (m_hostPlayerId == 0) {
        m_hostPlayerId = id;
        hostChanged = true;
        spdlog::info("GameServer: Player '{}' is the HOST (ID: {}, loopback={})",
                     player.name, id, player.isLoopback);
    } else if (player.isLoopback) {
        auto curHostIt = m_players.find(m_hostPlayerId);
        if (curHostIt != m_players.end() && !curHostIt->second.isLoopback) {
            spdlog::info("GameServer: Loopback peer '{}' (ID: {}) overrides non-loopback host '{}'",
                         player.name, id, curHostIt->second.name);
            m_hostPlayerId = id;
            hostChanged = true;
        }
    }

    spdlog::info("GameServer: Player '{}' joined (ID: {}, {} players now)",
                 player.name, id, m_players.size());

    // ES: Envía el HandshakeAck (ID asignado, tick, hora, clima, plazas). Si no se puede crear el
    //     paquete se deshace el alta del jugador.
    // EN: Send handshake ack
    {
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_HandshakeAck);
        MsgHandshakeAck ack{};
        ack.playerId = id;
        ack.serverTick = m_serverTick;
        ack.timeOfDay = m_timeOfDay;
        ack.weatherState = m_weatherState;
        ack.maxPlayers = static_cast<uint8_t>(m_config.maxPlayers);
        ack.currentPlayers = static_cast<uint8_t>(m_players.size());
        writer.WriteRaw(&ack, sizeof(ack));

        ENetPacket* pkt = enet_packet_create(writer.Data(), writer.Size(), ENET_PACKET_FLAG_RELIABLE);
        if (!pkt) {
            spdlog::error("Failed to create packet ({} bytes)", writer.Size());
            m_players.erase(id);
            peer->data = nullptr;
            return;
        }
        enet_peer_send(peer, KMP_CHANNEL_RELIABLE_ORDERED, pkt);
    }

    if (hostChanged) {
        BroadcastHostAssignment();
    }

    // ES: Envía la asignación de facción al jugador nuevo.
    // EN: Send faction assignment to the new player.
    // ES: Cada jugador recibe un StringId de facción del manifiesto faction-slots.json
    // (generado por ModGen a partir de kenshi-online.mod). El cliente parchea ese string
    // en .rdata antes de cargar el save para determinar qué facción controla.
    // El slot se calcula según factionMode (single/teams/per-player) en ComputeFactionSlot.
    // EN: Each player gets a faction StringId from the faction-slots.json manifest (generated by
    //     ModGen from kenshi-online.mod). The client patches that string into .rdata before loading
    //     the save to decide which faction it controls. The slot depends on factionMode
    //     (single/teams/per-player) in ComputeFactionSlot.
    {
        int slot = ComputeFactionSlot(id);
        // ES: SendFactionAssignment puede fallar si el manifiesto está vacío; en ese caso no
        // abortamos el handshake (el jugador sigue conectado, simplemente sin facción extra).
        // EN: SendFactionAssignment may fail if the manifest is empty; in that case the handshake is not aborted (the player stays connected, just without a faction).
        SendFactionAssignment(*GetPlayer(id), slot);
    }

    // ES: Avisa a los jugadores existentes del nuevo (con el nombre saneado).
    // EN: Notify existing players about the new player (use sanitized name)
    {
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_PlayerJoined);
        MsgPlayerJoined joined{};
        joined.playerId = id;
        strncpy(joined.playerName, sanitizedName.c_str(), KMP_MAX_NAME_LENGTH - 1);
        joined.playerName[KMP_MAX_NAME_LENGTH - 1] = '\0';
        writer.WriteRaw(&joined, sizeof(joined));
        BroadcastExcept(id, writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
    }

    // ES: Envía al nuevo la lista de jugadores que ya estaban.
    // EN: Send existing players to the new player
    for (auto& [existingId, existingPlayer] : m_players) {
        if (existingId == id) continue;

        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_PlayerJoined);
        MsgPlayerJoined joined{};
        joined.playerId = existingId;
        strncpy(joined.playerName, existingPlayer.name.c_str(), KMP_MAX_NAME_LENGTH - 1);
        joined.playerName[KMP_MAX_NAME_LENGTH - 1] = '\0';
        writer.WriteRaw(&joined, sizeof(joined));

        ENetPacket* pkt = enet_packet_create(writer.Data(), writer.Size(), ENET_PACKET_FLAG_RELIABLE);
        if (!pkt) {
            spdlog::error("Failed to create packet ({} bytes)", writer.Size());
            continue;
        }
        enet_peer_send(peer, KMP_CHANNEL_RELIABLE_ORDERED, pkt);
    }

    // ES: Envía al nuevo la foto completa del mundo (entidades + equipo).
    // EN: Send world snapshot to new player
    SendWorldSnapshot(m_players[id]);

    BroadcastSystemMessage(player.name + " joined the game");
}

// ES: Consulta del navegador de servidores (C2S_ServerQuery): responde S2C_ServerInfo (nombre,
//     jugadores, plazas, puerto, hora, PvP) sin handshake y después desconecta a ese peer.
// EN: Server browser query (C2S_ServerQuery): replies with S2C_ServerInfo (name, players, slots,
//     port, time, PvP) without a handshake and then disconnects that peer.
void GameServer::HandleServerQuery(ENetPeer* peer, PacketReader& reader) {
    // ES: Consulta ligera: permite al navegador mostrar el número real de jugadores. m_mutex ya lo tiene el llamador.
    // EN: Lightweight query — respond with server info without requiring handshake.
    // This lets the server browser show real player counts.
    // Note: m_mutex must be held (acquired by HandlePacket caller)
    MsgServerQuery query;
    if (!reader.ReadRaw(&query, sizeof(query))) return;

    spdlog::debug("GameServer: Server query from peer (protocol={})", query.protocolVersion);

    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_ServerInfo);

    MsgServerInfo info{};
    info.protocolVersion = KMP_PROTOCOL_VERSION;
    info.currentPlayers = static_cast<uint8_t>(m_players.size());
    info.maxPlayers = static_cast<uint8_t>(m_config.maxPlayers);
    info.port = m_config.port;
    info.timeOfDay = m_timeOfDay;
    info.pvpEnabled = m_config.pvpEnabled ? 1 : 0;
    strncpy(info.serverName, m_config.serverName.c_str(), sizeof(info.serverName) - 1);
    info.serverName[sizeof(info.serverName) - 1] = '\0';
    writer.WriteRaw(&info, sizeof(info));

    ENetPacket* pkt = enet_packet_create(writer.Data(), writer.Size(), ENET_PACKET_FLAG_RELIABLE);
    if (!pkt) {
        spdlog::error("Failed to create packet ({} bytes)", writer.Size());
        enet_peer_disconnect_later(peer, 0);
        return;
    }
    enet_peer_send(peer, KMP_CHANNEL_RELIABLE_ORDERED, pkt);

    // ES: Tras responder se desconecta al peer (no va a entrar a jugar).
    // EN: Disconnect the query peer after responding (they're not joining)
    enet_peer_disconnect_later(peer, 0);
}

// ES: C2S_PositionUpdate (canal no fiable): [u8 count] + count * CharacterPosition. Por cada
//     entidad: descarta coordenadas NaN/inf/> 1e6, valida autoridad (el jugador debe ser dueño) y
//     actualiza posición, rotación, zona y animación en el servidor. La primera entidad válida fija
//     la posición/zona del jugador. No reenvía nada: BroadcastPositions lo reparte en el tick.
// EN: C2S_PositionUpdate (unreliable channel): [u8 count] + count * CharacterPosition. For each
//     entity: drops NaN/inf/> 1e6 coordinates, validates authority (the player must own it) and
//     updates position, rotation, zone and animation server-side. The first valid entity sets the
//     player position/zone. Nothing is relayed here: BroadcastPositions spreads it on the tick.
void GameServer::HandlePositionUpdate(ConnectedPlayer& player, PacketReader& reader) {
    uint8_t count;
    if (!reader.ReadU8(count)) return;

    // ES: Anti-flood: descarta el paquete entero si el jugador supera el umbral de updates/seg.
    // (Cada paquete puede traer varias entidades, pero limitamos por paquete recibido.)
    // EN: Anti-flood: drop the whole packet if the player exceeds the updates/second threshold.
    //     (A packet can carry several entities, but the limit is per received packet.)
    if (!CheckRateLimit(player, player.posMsgCount, KMP_MAX_POS_PER_SEC)) {
        spdlog::debug("GameServer: Position update de '{}' descartado por rate limit", player.name);
        return;
    }

    bool playerPosUpdated = false;
    for (uint8_t i = 0; i < count; i++) {
        CharacterPosition pos;
        if (!reader.ReadRaw(&pos, sizeof(pos))) break;

        // ES: Valida coordenadas: rechaza NaN/inf/valores extremos para no hacer petar a los clientes.
        // EN: Validate coordinates — reject NaN/inf/extreme values to prevent client crashes
        if (std::isnan(pos.posX) || std::isnan(pos.posY) || std::isnan(pos.posZ) ||
            std::isinf(pos.posX) || std::isinf(pos.posY) || std::isinf(pos.posZ) ||
            std::abs(pos.posX) > 1000000.f || std::abs(pos.posY) > 1000000.f ||
            std::abs(pos.posZ) > 1000000.f) {
            spdlog::warn("GameServer: Rejected invalid position from '{}' entity {} ({},{},{})",
                         player.name, pos.entityId, pos.posX, pos.posY, pos.posZ);
            continue;
        }

        // ES: === VALIDACIÓN DE AUTORIDAD DEL SERVIDOR (Fase 5) ===
        //     Rechaza updates de entidades que el cliente no posee (el validador ya registra el motivo).
        // EN: === SERVER AUTHORITY VALIDATION (Phase 5) ===
        // Reject updates for entities the client doesn't own
        if (!ServerAuthorityValidator::ValidatePositionUpdate(player.id, pos.entityId, m_entities)) {
            continue; // Authority violation logged inside validator
        }

        // ES: Actualiza la entidad en el servidor (posición, rotación descomprimida, zona, animación).
        // EN: Update server-side entity position
        auto it = m_entities.find(pos.entityId);
        if (it != m_entities.end()) {
            it->second.position = Vec3(pos.posX, pos.posY, pos.posZ);
            it->second.rotation = Quat::Decompress(pos.compressedQuat);
            it->second.zone = ZoneCoord::FromWorldPos(it->second.position);
            it->second.animState = pos.animStateId;
            it->second.moveSpeed = pos.moveSpeed;
            it->second.flags = pos.flags;

            // ES: La primera entidad propia marca la posición/zona del jugador.
            // EN: Use the first owned entity's position for zone tracking
            if (!playerPosUpdated) {
                player.position = Vec3(pos.posX, pos.posY, pos.posZ);
                player.zone = ZoneCoord::FromWorldPos(player.position);
                playerPosUpdated = true;
            }
        }
    }
}

// ES: C2S_MoveCommand: orden de movimiento de una entidad propia. Tras validar autoridad se reenvía
//     tal cual a los demás (S2C_MoveCommand). La variable 'it' no se usa.
// EN: C2S_MoveCommand: move order for an owned entity. After the authority check it is relayed
//     as-is to the others (S2C_MoveCommand). The 'it' variable is unused.
void GameServer::HandleMoveCommand(ConnectedPlayer& player, PacketReader& reader) {
    MsgMoveCommand msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: === VALIDACIÓN DE AUTORIDAD DEL SERVIDOR (Fase 5) ===
    // EN: === SERVER AUTHORITY VALIDATION (Phase 5) ===
    if (!ServerAuthorityValidator::CanClientCommandEntity(player.id, msg.entityId, m_entities)) {
        return; // Authority violation logged
    }

    auto it = m_entities.find(msg.entityId);

    // ES: Reenvía al resto de jugadores.
    // EN: Broadcast to other players
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_MoveCommand);
    writer.WriteRaw(&msg, sizeof(msg));
    BroadcastExcept(player.id, writer.Data(), writer.Size(),
                   KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: C2S_AttackIntent: un jugador ataca con una entidad propia. Valida autoridad del atacante,
//     que el objetivo exista y viva y la distancia (cuerpo a cuerpo < 15 m, a distancia < 150 m si
//     attackType == 1). Resuelve el daño con ResolveCombat (el servidor manda) y envía a TODOS
//     S2C_CombatHit y, si procede, S2C_CombatDeath o S2C_CombatKO.
// EN: C2S_AttackIntent: a player attacks with an owned entity. Validates attacker authority, that
//     the target exists and is alive, and the distance (melee < 15 m, ranged < 150 m when
//     attackType == 1). Resolves damage with ResolveCombat (server is authoritative) and sends
//     S2C_CombatHit to EVERYONE plus S2C_CombatDeath or S2C_CombatKO when applicable.
void GameServer::HandleAttackIntent(ConnectedPlayer& player, PacketReader& reader) {
    MsgAttackIntent msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: === VALIDACIÓN DE AUTORIDAD DEL SERVIDOR (Fase 5) ===
    // EN: === SERVER AUTHORITY VALIDATION (Phase 5) ===
    if (!ServerAuthorityValidator::CanClientCommandEntity(player.id, msg.attackerId, m_entities)) {
        return; // Authority violation logged
    }

    auto it = m_entities.find(msg.attackerId);

    // ES: El objetivo debe existir y estar vivo.
    // EN: Validate target exists and is alive
    auto targetIt = m_entities.find(msg.targetId);
    if (targetIt == m_entities.end() || !targetIt->second.alive) return;

    // ES: Validación de distancia: cuerpo a cuerpo < 15 m, a distancia < 150 m.
    // EN: Distance validation: melee < 15m, ranged < 150m
    float dx = it->second.position.x - targetIt->second.position.x;
    float dy = it->second.position.y - targetIt->second.position.y;
    float dz = it->second.position.z - targetIt->second.position.z;
    float distSq = dx*dx + dy*dy + dz*dz;
    float maxDist = (msg.attackType == 1) ? 150.f : 15.f;
    if (distSq > maxDist * maxDist) return;

    // ES: Delega en el resolvedor de combate (única fuente de verdad de la lógica de combate).
    // EN: Delegate to combat resolver (single source of truth for combat logic)
    auto result = ResolveCombat(it->second, targetIt->second, msg.attackType);

    // ES: Construye el golpe para enviarlo.
    // EN: Build and broadcast hit
    MsgCombatHit hit{};
    hit.attackerId = msg.attackerId;
    hit.targetId = msg.targetId;
    hit.bodyPart = static_cast<uint8_t>(result.hitPart);
    hit.cutDamage = result.cutDamage;
    hit.bluntDamage = result.bluntDamage;
    hit.pierceDamage = result.pierceDamage;
    hit.resultHealth = result.resultHealth;
    hit.wasBlocked = result.wasBlocked ? 1 : 0;
    hit.wasKO = result.wasKO ? 1 : 0;

    // ES: Se envía el golpe a TODOS (atacante incluido): el atacante ya aplicó daño local, pero el
    //     resultado del servidor manda y el cliente lo reconcilia con estos valores.
    // EN: Broadcast hit to ALL players (including attacker) for authoritative sync.
    // The attacker already applied local damage, but the server's result is
    // authoritative — the client will reconcile using the server values.
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_CombatHit);
    writer.WriteRaw(&hit, sizeof(hit));
    Broadcast(writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);

    if (result.wasDeath) {
        // ES: Marca la entidad como muerta ANTES de avisar (el servidor manda).
        // EN: Mark entity as dead BEFORE broadcast (server is authoritative)
        targetIt->second.alive = false;

        PacketWriter deathWriter;
        deathWriter.WriteHeader(MessageType::S2C_CombatDeath);
        MsgCombatDeath death{};
        death.entityId = msg.targetId;
        death.killerId = msg.attackerId;
        deathWriter.WriteRaw(&death, sizeof(death));
        Broadcast(deathWriter.Data(), deathWriter.Size(),
                 KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);

        spdlog::info("GameServer: Entity {} killed by {} (owned by {})",
                     msg.targetId, msg.attackerId, player.name);
    } else if (result.wasKO) {
        PacketWriter koWriter;
        koWriter.WriteHeader(MessageType::S2C_CombatKO);
        MsgCombatKO ko{};
        ko.entityId = msg.targetId;
        ko.attackerId = msg.attackerId;
        ko.bodyPart = static_cast<uint8_t>(result.hitPart);
        ko.resultHealth = result.resultHealth;
        koWriter.WriteRaw(&ko, sizeof(ko));
        Broadcast(koWriter.Data(), koWriter.Size(),
                 KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
    }
}

// ES: C2S_ChatMessage: lee y descarta el senderId del paquete (se usa el ID de la conexión, fiable),
//     limita a 256 caracteres y 5 mensajes/s y reenvía S2C_ChatMessage a los demás (sin eco).
// EN: C2S_ChatMessage: reads and discards the packet senderId (the connection ID is trusted instead),
//     caps at 256 characters and 5 messages/s and relays S2C_ChatMessage to others (no echo).
void GameServer::HandleChatMessage(ConnectedPlayer& player, PacketReader& reader) {
    // ES: El cliente envía U32(senderId) + U16(len) + texto; el senderId se ignora.
    // EN: Client sends: U32(senderId) + U16(len) + Raw(text).
    // Read and discard senderId — use player.id from the connection instead (trusted).
    uint32_t senderId;
    if (!reader.ReadU32(senderId)) return;
    std::string message;
    // ES: Cap de longitud de chat más bajo (256) para evitar spam de mensajes enormes.
    // EN: Lower chat length cap (256) to avoid spam of huge messages.
    if (!reader.ReadString(message, KMP_MAX_CHAT_LEN)) return;
    if (message.empty()) return;

    // ES: Anti-flood: descarta si el jugador supera el umbral de chat/seg.
    // EN: Anti-flood: drop it if the player exceeds the chat/second threshold.
    if (!CheckRateLimit(player, player.chatMsgCount, KMP_MAX_CHAT_PER_SEC)) {
        spdlog::debug("GameServer: Chat de '{}' descartado por rate limit", player.name);
        return;
    }

    spdlog::info("[Chat] {}: {}", player.name, message);

    // ES: Reenvía a los DEMÁS jugadores (sin eco al emisor).
    // EN: Broadcast to all OTHER players (don't echo back to sender)
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_ChatMessage);
    writer.WriteU32(player.id);
    writer.WriteString(message);
    BroadcastExcept(player.id, writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: C2S_BuildRequest: colocar un edificio. Valida coordenadas y que esté a <= 500 m de la última
//     posición conocida del jugador; crea una entidad Building propiedad del jugador y avisa a los
//     demás con S2C_BuildPlaced (el constructor no recibe el ID del servidor por este mensaje).
// EN: C2S_BuildRequest: place a building. Validates coordinates and that it is within 500 m of the
//     player last known position; creates a Building entity owned by the player and notifies the
//     others with S2C_BuildPlaced (the builder does not get the server ID through this message).
void GameServer::HandleBuildRequest(ConnectedPlayer& player, PacketReader& reader) {
    MsgBuildRequest msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: Valida coordenadas de construcción (NaN/inf/extremas).
    // EN: Validate build coordinates — reject NaN/inf/extreme values
    if (std::isnan(msg.posX) || std::isnan(msg.posY) || std::isnan(msg.posZ) ||
        std::isinf(msg.posX) || std::isinf(msg.posY) || std::isinf(msg.posZ) ||
        std::abs(msg.posX) > 1000000.f || std::abs(msg.posY) > 1000000.f ||
        std::abs(msg.posZ) > 1000000.f) {
        spdlog::warn("GameServer: Rejected invalid build position from '{}' ({},{},{})",
                     player.name, msg.posX, msg.posY, msg.posZ);
        return;
    }

    // ES: La construcción debe estar a una distancia razonable del jugador (evita construir al otro lado del mapa con un cliente modificado).
    // EN: Validate build position is within reasonable distance of player's known position
    // (prevents building across the map via modified client)
    constexpr float MAX_BUILD_DISTANCE = 500.f;
    float dx = msg.posX - player.position.x;
    float dy = msg.posY - player.position.y;
    float dz = msg.posZ - player.position.z;
    float distSq = dx*dx + dy*dy + dz*dz;
    if (distSq > MAX_BUILD_DISTANCE * MAX_BUILD_DISTANCE) {
        spdlog::warn("GameServer: Rejected build from '{}' too far from player ({:.0f}m away)",
                     player.name, std::sqrt(distSq));
        return;
    }

    // ES: Crea la entidad edificio.
    // EN: Create building entity
    EntityID buildId = m_nextEntityId++;
    ServerEntity building;
    building.id = buildId;
    building.type = EntityType::Building;
    building.owner = player.id;
    building.position = Vec3(msg.posX, msg.posY, msg.posZ);
    building.rotation = Quat::Decompress(msg.compressedQuat);
    building.zone = ZoneCoord::FromWorldPos(building.position);
    building.templateId = msg.templateId;
    building.alive = true;
    m_entities[buildId] = building;

    // ES: Avisa de la colocación.
    // EN: Broadcast placement
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_BuildPlaced);
    MsgBuildPlaced placed{};
    placed.entityId = buildId;
    placed.templateId = msg.templateId;
    placed.posX = msg.posX;
    placed.posY = msg.posY;
    placed.posZ = msg.posZ;
    placed.compressedQuat = msg.compressedQuat;
    placed.builderId = player.id;
    writer.WriteRaw(&placed, sizeof(placed));
    BroadcastExcept(player.id, writer.Data(), writer.Size(),
                    KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);

    spdlog::info("GameServer: Player '{}' placed building {} at ({:.1f}, {:.1f}, {:.1f})",
                 player.name, buildId, msg.posX, msg.posY, msg.posZ);
}

// ES: Cada tick, por jugador: envía por el canal no fiable las posiciones de TODAS las entidades que
//     no son suyas (máx. 255 por paquete: las demás no se envían en ese tick). Formato:
//     [header][u32 sourcePlayer = 0][u8 count][CharacterPosition...].
// EN: Every tick, per player: sends over the unreliable channel the positions of ALL entities it does
//     not own (max 255 per packet: the rest are not sent that tick). Format:
//     [header][u32 sourcePlayer = 0][u8 count][CharacterPosition...].
void GameServer::BroadcastPositions() {
    // ES: Recoge posiciones y las envía a los jugadores relevantes (en origen, por zonas de interés).
    // EN: Collect all entity positions and broadcast to relevant players
    // (zone-based interest management)
    for (auto& [playerId, player] : m_players) {
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_PositionUpdate);

        // ES: Recoge todas las entidades ajenas. El filtrado por zona está desactivado a propósito: con
        //     16 plazas no hace falta y el desajuste de zonas impedía que los jugadores se vieran.
        // EN: Collect all non-owned entities (zone filtering disabled for small
        // player counts — 16-slot server doesn't need spatial culling, and
        // zone mismatch was preventing players from ever seeing each other).
        std::vector<const ServerEntity*> nearby;
        for (auto& [entityId, entity] : m_entities) {
            if (entity.owner == playerId) continue; // Don't send own entities back
            nearby.push_back(&entity);
        }

        if (nearby.empty()) continue;

        writer.WriteU32(0); // sourcePlayer = server
        size_t sendCount = std::min(nearby.size(), size_t(255));
        writer.WriteU8(static_cast<uint8_t>(sendCount));

        for (size_t i = 0; i < sendCount; i++) {
            auto* entity = nearby[i];
            CharacterPosition pos;
            pos.entityId = entity->id;
            pos.generation = entity->generation; // Phase 6: include generation
            pos.posX = entity->position.x;
            pos.posY = entity->position.y;
            pos.posZ = entity->position.z;
            pos.compressedQuat = entity->rotation.Compress();
            pos.animStateId = entity->animState;
            pos.moveSpeed = entity->moveSpeed;
            pos.flags = entity->flags;
            writer.WriteRaw(&pos, sizeof(pos));
        }

        ENetPacket* pkt = enet_packet_create(writer.Data(), writer.Size(), 0);
        if (!pkt) {
            spdlog::error("Failed to create packet ({} bytes)", writer.Size());
            continue;
        }
        enet_peer_send(player.peer, KMP_CHANNEL_UNRELIABLE_SEQ, pkt);
    }
}

// ES: Envía S2C_TimeSync a todos (canal fiable): tick, hora, clima y velocidad (0 si está en pausa).
//     Se llama cada 5 s y al instante cuando cambian velocidad, pausa, hora o clima.
// EN: Sends S2C_TimeSync to everyone (reliable channel): tick, time, weather and speed (0 when
//     paused). Called every 5 s and immediately whenever speed, pause, time or weather change.
void GameServer::BroadcastTimeSync() {
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_TimeSync);
    MsgTimeSync msg;
    msg.serverTick = m_serverTick;
    msg.timeOfDay = m_timeOfDay;
    msg.weatherState = m_weatherState;
    // ES: Velocidad efectiva: si el servidor está pausado, enviamos 0 a los clientes.
    // El formato del paquete NO cambia (gameSpeed sigue siendo float, el cliente ya lo
    // parsea como float) — solo cambia el VALOR que pone el servidor.
    // EN: Effective speed: if the server is paused, 0 is sent to clients. The packet format does NOT
    //     change (gameSpeed is still a float the client already parses); only the VALUE changes.
    msg.gameSpeed = m_serverPaused ? 0.f : m_config.gameSpeed;
    writer.WriteRaw(&msg, sizeof(msg));
    Broadcast(writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: Avisa a todos de quién es el host (S2C_HostAssignment con newHostPlayerId).
// EN: Tells everyone who the host is (S2C_HostAssignment with newHostPlayerId).
void GameServer::BroadcastHostAssignment() {
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_HostAssignment);
    MsgHostAssignment msg{};
    msg.newHostPlayerId = m_hostPlayerId;
    writer.WriteRaw(&msg, sizeof(msg));
    Broadcast(writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
    spdlog::info("GameServer: Broadcast S2C_HostAssignment (newHost={})", m_hostPlayerId);
}

// ES: Heartbeat anti-fantasmas, por jugador: [u32 tick][u16 n][u32 id...] con las entidades activas
//     que DEBERÍAN existir para él (suyas, en zona adyacente o del servidor). El cliente compara con
//     su registro y borra las que el servidor ya no conoce.
// EN: Anti-ghost heartbeat, per player: [u32 tick][u16 n][u32 id...] with the active entities that
//     SHOULD exist for it (its own, in an adjacent zone or server-owned). The client compares with
//     its registry and removes those the server no longer knows.
void GameServer::BroadcastEntityHeartbeat() {
    // ES: Heartbeat por jugador: cada cliente recibe los IDs que deberían existir en su zona de interés
    //     y limpia las entidades que el servidor ya no conoce (fantasmas por desconexiones silenciosas,
    //     cambios de zona o despawns perdidos).
    // EN: Send per-player heartbeat: each client gets the list of entity IDs that
    // should exist in their interest zone. Client compares against local registry
    // and cleans up entities the server no longer knows about (ghost entities from
    // silent disconnects, zone changes, or missed despawn messages).
    for (auto& [playerId, player] : m_players) {
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_EntityHeartbeat);
        writer.WriteU32(m_serverTick);

        // ES: Recoge los IDs relevantes para este jugador.
        // EN: Collect entity IDs relevant to this player
        std::vector<EntityID> relevantIds;
        for (const auto& [entityId, entity] : m_entities) {
            if (entity.state != EntityState::Active) continue;

            // ES: Incluye: propias, en zonas cercanas o del servidor (owner 0).
            // EN: Include: entities owned by this player, or in nearby zones
            if (entity.owner == playerId ||
                entity.zone.IsAdjacent(player.zone) ||
                entity.owner == 0) {
                relevantIds.push_back(entityId);
            }
        }

        writer.WriteU16(static_cast<uint16_t>(relevantIds.size()));
        for (EntityID id : relevantIds) {
            writer.WriteU32(id);
        }

        SendTo(playerId, writer.Data(), writer.Size(),
               KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
    }
}

// ES: C2S_EntitySpawnReq: el cliente avisa de que se creó un personaje en su juego. Formato:
//     u32 idCliente, u8 tipo, u32 dueño (ignorado: se usa el jugador), u32 plantilla, vec3 posición,
//     u32 quat comprimido, u32 facción, [u16 len + nombre de plantilla], [u8 ext=1 + 7 f32 vida +
//     u8 vivo]. El servidor asigna un ID propio, lo guarda y envía S2C_EntitySpawn a TODOS (también
//     al emisor, para que conozca el ID del servidor). Límite global KMP_MAX_SYNC_ENTITIES.
// EN: C2S_EntitySpawnReq: the client reports a character was created in its game. Format:
//     u32 clientId, u8 type, u32 owner (ignored: the sender is used), u32 template, vec3 position,
//     u32 compressed quat, u32 faction, [u16 len + template name], [u8 ext=1 + 7 f32 health +
//     u8 alive]. The server assigns its own ID, stores it and sends S2C_EntitySpawn to EVERYONE
//     (sender included, so it learns the server ID). Global cap KMP_MAX_SYNC_ENTITIES.
void GameServer::HandleEntitySpawnReq(ConnectedPlayer& player, PacketReader& reader) {
    // ES: El cliente informa de un personaje nuevo; el servidor le da ID, lo guarda y lo difunde.
    // EN: Host client reports a character was created in-game. Server assigns a server
    // entity ID, stores it, and broadcasts S2C_EntitySpawn to all clients.
    uint32_t clientEntityId, templateId, factionId;
    uint8_t type;
    uint32_t ownerId;
    float px, py, pz;
    uint32_t compQuat;

    if (!reader.ReadU32(clientEntityId)) return;
    if (!reader.ReadU8(type)) return;
    if (!reader.ReadU32(ownerId)) return;
    if (!reader.ReadU32(templateId)) return;
    if (!reader.ReadVec3(px, py, pz)) return;
    if (!reader.ReadU32(compQuat)) return;
    if (!reader.ReadU32(factionId)) return;

    // ES: Nombre de plantilla opcional (hasta 255 bytes).
    // EN: Read optional template name
    std::string templateName;
    uint16_t nameLen = 0;
    if (reader.Remaining() >= 2) {
        reader.ReadU16(nameLen);
        if (nameLen > 0 && nameLen <= 255 && reader.Remaining() >= nameLen) {
            templateName.resize(nameLen);
            reader.ReadRaw(templateName.data(), nameLen);
        }
    }

    // ES: Estado extendido opcional (vida + vivo), detrás del nombre de plantilla.
    // EN: Read optional extended state (health + alive flag, appended after template name)
    bool hasExtended = false;
    float healthData[7] = {100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f};
    bool isAlive = true;
    if (reader.Remaining() >= 1) {
        uint8_t extFlag = 0;
        reader.ReadU8(extFlag);
        if (extFlag == 1 && reader.Remaining() >= 7 * 4 + 1) {
            hasExtended = true;
            for (int i = 0; i < 7; i++) reader.ReadF32(healthData[i]);
            uint8_t aliveFlag = 1;
            reader.ReadU8(aliveFlag);
            isAlive = (aliveFlag != 0);
        }
    }

    // ES: Valida el tipo de entidad.
    // EN: Validate entity type
    if (type > static_cast<uint8_t>(EntityType::Turret)) {
        spdlog::warn("GameServer: Invalid entity type {} from player '{}'", type, player.name);
        return;
    }

    // ES: Aplica el límite global de entidades.
    // EN: Enforce global entity limit
    if (m_entities.size() >= KMP_MAX_SYNC_ENTITIES) {
        spdlog::warn("GameServer: Entity limit reached ({}) — rejecting spawn from '{}'",
                      m_entities.size(), player.name);
        return;
    }

    // ES: Asigna el ID de entidad del servidor.
    // EN: Assign server entity ID
    EntityID serverId = m_nextEntityId++;

    // ES: Guarda la entidad (authority se queda en Server, ver nota en server.h).
    // EN: Store in server entity list
    ServerEntity entity;
    entity.id = serverId;
    entity.type = static_cast<EntityType>(type);
    entity.owner = player.id;
    entity.position = Vec3(px, py, pz);
    entity.rotation = Quat::Decompress(compQuat);
    entity.templateId = templateId;
    entity.factionId = factionId;
    entity.templateName = templateName;
    entity.zone = ZoneCoord::FromWorldPos(entity.position, KMP_ZONE_SIZE);
    if (hasExtended) {
        for (int i = 0; i < 7; i++) entity.health[i] = healthData[i];
        entity.alive = isAlive;
    }
    m_entities[serverId] = entity;

    // ES: Inicializa la posición/zona del jugador con su primera entidad para que las zonas de interés funcionen antes de que llegue ninguna posición.
    // EN: Initialize player position/zone from their first entity so zone-based
    // interest management works before the player sends any position updates
    if (player.zone == ZoneCoord(0, 0)) {
        player.position = entity.position;
        player.zone = entity.zone;
    }

    spdlog::info("GameServer: Entity spawn req from '{}': serverID={} template='{}' at ({:.1f},{:.1f},{:.1f})",
                 player.name, serverId, templateName, px, py, pz);

    // ES: Envía S2C_EntitySpawn a TODOS los clientes (incluido el emisor, para que reciba el ID del servidor).
    // EN: Broadcast S2C_EntitySpawn to ALL clients (including the host, so they get the server ID)
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_EntitySpawn);
    writer.WriteU32(serverId);
    writer.WriteU8(type);
    writer.WriteU32(player.id);
    writer.WriteU32(templateId);
    writer.WriteF32(px);
    writer.WriteF32(py);
    writer.WriteF32(pz);
    writer.WriteU32(compQuat);
    writer.WriteU32(factionId);
    uint16_t broadcastNameLen = static_cast<uint16_t>(templateName.size());
    writer.WriteU16(broadcastNameLen);
    if (broadcastNameLen > 0) {
        writer.WriteRaw(templateName.data(), broadcastNameLen);
    }

    // ES: Siempre se incluye el estado extendido para que el formato sea igual en SpawnReq, WorldSnapshot y ZoneRequest.
    // EN: Always relay extended state (health + alive) so packet format is consistent
    // across HandleEntitySpawnReq, SendWorldSnapshot, and HandleZoneRequest
    writer.WriteU8(1); // hasExtendedState
    for (int i = 0; i < 7; i++) writer.WriteF32(healthData[i]);
    writer.WriteU8(isAlive ? 1 : 0);

    Broadcast(writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: C2S_EntityDespawnReq: el dueño pide quitar una entidad. Se borra del servidor y se avisa a
//     todos con S2C_EntityDespawn. OJO: si falta el byte opcional 'reason', la variable queda sin
//     inicializar y se reenvía un valor basura (sin efecto de seguridad, pero es un fallo menor).
// EN: C2S_EntityDespawnReq: the owner asks to remove an entity. It is deleted server-side and
//     everyone gets S2C_EntityDespawn. NOTE: if the optional 'reason' byte is missing, the variable
//     stays uninitialized and a garbage value is relayed (no security impact, but a minor bug).
void GameServer::HandleEntityDespawnReq(ConnectedPlayer& player, PacketReader& reader) {
    uint32_t entityId;
    uint8_t reason;
    if (!reader.ReadU32(entityId)) return;
    reader.ReadU8(reason); // optional

    // ES: La entidad debe existir y ser del jugador.
    // EN: Validate: entity must exist and be owned by this player
    auto it = m_entities.find(entityId);
    if (it == m_entities.end()) return;
    if (it->second.owner != player.id) {
        spdlog::warn("GameServer: Player '{}' tried to despawn entity {} they don't own", player.name, entityId);
        return;
    }

    spdlog::info("GameServer: Entity {} despawned by '{}' (reason={})", entityId, player.name, reason);

    // ES: Se quita del servidor.
    // EN: Remove from server
    m_entities.erase(it);

    // ES: Aviso de despawn a todos los clientes.
    // EN: Broadcast despawn to all clients
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_EntityDespawn);
    writer.WriteU32(entityId);
    writer.WriteU8(reason);
    Broadcast(writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: C2S_EquipmentUpdate: cambio de equipo (ranura 0-13 -> ID de plantilla del objeto) de una entidad
//     propia. Se guarda (para snapshots/guardado) y se reenvía a los demás.
// EN: C2S_EquipmentUpdate: equipment change (slot 0-13 -> item template ID) for an owned entity.
//     Stored (for snapshots/saving) and relayed to the others.
void GameServer::HandleEquipmentUpdate(ConnectedPlayer& player, PacketReader& reader) {
    MsgEquipmentUpdate msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: La entidad debe existir y ser del jugador.
    // EN: Validate entity exists and is owned by this player
    auto it = m_entities.find(msg.entityId);
    if (it == m_entities.end() || it->second.owner != player.id) return;

    // ES: Ranura válida (14 ranuras).
    // EN: Validate slot
    if (msg.slot >= 14) return;

    // ES: Actualiza el estado del servidor.
    // EN: Update server state
    it->second.equipment[msg.slot] = msg.itemTemplateId;

    spdlog::debug("GameServer: Equipment update from '{}': entity={} slot={} item={}",
                  player.name, msg.entityId, msg.slot, msg.itemTemplateId);

    // ES: Reenvía al resto de jugadores.
    // EN: Broadcast to all other players
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_EquipmentUpdate);
    writer.WriteRaw(&msg, sizeof(msg));
    BroadcastExcept(player.id, writer.Data(), writer.Size(),
                   KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: C2S_ZoneRequest (i32 x, i32 y): el cliente pide las entidades de una zona. Se le envían, como
//     S2C_EntitySpawn, todas las entidades ajenas de esa zona y sus adyacentes.
// EN: C2S_ZoneRequest (i32 x, i32 y): the client asks for a zone entities. It gets, as
//     S2C_EntitySpawn, every non-owned entity in that zone and its neighbours.
void GameServer::HandleZoneRequest(ConnectedPlayer& player, PacketReader& reader) {
    int32_t zoneX, zoneY;
    if (!reader.ReadI32(zoneX) || !reader.ReadI32(zoneY)) return;

    // ES: Validación de límites: el mundo de Kenshi mide aprox. de -100 km a +100 km; a 750 m por zona
    //     son unas ±133 zonas. Se acepta hasta ±500 por seguridad.
    // EN: Zone bounds validation: Kenshi's world is roughly -100km to +100km.
    // At 750m per zone, that's about ±133 zones. Clamp to ±500 to be safe.
    constexpr int32_t ZONE_MAX = 500;
    if (zoneX < -ZONE_MAX || zoneX > ZONE_MAX || zoneY < -ZONE_MAX || zoneY > ZONE_MAX) {
        spdlog::warn("GameServer: Player '{}' sent invalid zone ({}, {}) — out of bounds", player.name, zoneX, zoneY);
        return;
    }

    spdlog::debug("GameServer: Player '{}' requested zone ({}, {})", player.name, zoneX, zoneY);

    ZoneCoord requestedZone(zoneX, zoneY);

    // ES: Envía las entidades de la zona pedida y de las adyacentes.
    // EN: Send all entities in the requested zone (and adjacent zones) to this player
    for (auto& [entityId, entity] : m_entities) {
        if (entity.owner == player.id) continue; // Don't send own entities
        if (!requestedZone.IsAdjacent(entity.zone) && !(entity.zone.x == zoneX && entity.zone.y == zoneY))
            continue;

        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_EntitySpawn);
        writer.WriteU32(entity.id);
        writer.WriteU8(static_cast<uint8_t>(entity.type));
        writer.WriteU32(entity.owner);
        writer.WriteU32(entity.templateId);
        writer.WriteF32(entity.position.x);
        writer.WriteF32(entity.position.y);
        writer.WriteF32(entity.position.z);
        writer.WriteU32(entity.rotation.Compress());
        writer.WriteU32(entity.factionId);
        uint16_t nameLen = static_cast<uint16_t>(
            std::min<size_t>(entity.templateName.size(), 255));
        writer.WriteU16(nameLen);
        if (nameLen > 0) {
            writer.WriteRaw(entity.templateName.data(), nameLen);
        }

        // ES: Incluye el estado de vida.
        // EN: Include health state
        writer.WriteU8(1); // hasExtendedState
        for (int i = 0; i < 7; i++) writer.WriteF32(entity.health[i]);
        writer.WriteU8(entity.alive ? 1 : 0);

        ENetPacket* pkt = enet_packet_create(writer.Data(), writer.Size(), ENET_PACKET_FLAG_RELIABLE);
        if (!pkt) {
            spdlog::error("Failed to create packet ({} bytes)", writer.Size());
            continue;
        }
        enet_peer_send(player.peer, KMP_CHANNEL_RELIABLE_ORDERED, pkt);
    }
}

// ES: Foto del mundo para un jugador que acaba de entrar: un S2C_EntitySpawn por entidad (con nombre
//     de plantilla y vida) y un S2C_EquipmentUpdate por cada ranura de equipo ocupada. Salta las
//     entidades de jugadores desconectados y las de posición inválida.
// EN: World snapshot for a newly joined player: one S2C_EntitySpawn per entity (with template name
//     and health) and one S2C_EquipmentUpdate per occupied equipment slot. Skips entities of
//     disconnected players and those with invalid positions.
void GameServer::SendWorldSnapshot(ConnectedPlayer& player) {
    int sent = 0;
    int skippedOrphan = 0;
    int skippedPosition = 0;

    for (auto& [entityId, entity] : m_entities) {
        // ES: Salta entidades de jugadores desconectados (huérfanas de guardados viejos).
        // EN: Skip entities owned by disconnected players (orphans from stale saves)
        if (entity.owner != 0 && !GetPlayer(entity.owner)) {
            skippedOrphan++;
            continue;
        }

        // ES: Salta entidades con posiciones basura (NaN, inf o extremas).
        // EN: Skip entities with garbage positions (NaN, inf, or extreme values)
        if (std::isnan(entity.position.x) || std::isnan(entity.position.y) || std::isnan(entity.position.z) ||
            std::isinf(entity.position.x) || std::isinf(entity.position.y) || std::isinf(entity.position.z) ||
            std::abs(entity.position.x) > 1000000.f || std::abs(entity.position.y) > 1000000.f ||
            std::abs(entity.position.z) > 1000000.f) {
            skippedPosition++;
            continue;
        }

        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_EntitySpawn);
        writer.WriteU32(entity.id);
        writer.WriteU8(static_cast<uint8_t>(entity.type));
        writer.WriteU32(entity.owner);
        writer.WriteU32(entity.templateId);
        writer.WriteF32(entity.position.x);
        writer.WriteF32(entity.position.y);
        writer.WriteF32(entity.position.z);
        writer.WriteU32(entity.rotation.Compress());
        writer.WriteU32(entity.factionId);
        // ES: Añade el nombre de plantilla para que el cliente pueda crear el personaje con SpawnManager.
        // EN: Append template name so the client can spawn via SpawnManager
        uint16_t nameLen = static_cast<uint16_t>(
            std::min<size_t>(entity.templateName.size(), 255));
        writer.WriteU16(nameLen);
        if (nameLen > 0) {
            writer.WriteRaw(entity.templateName.data(), nameLen);
        }

        // ES: Incluye la vida para que el que entra vea bien el daño por miembro.
        // EN: Include health state so joining clients see correct limb damage
        writer.WriteU8(1); // hasExtendedState
        for (int i = 0; i < 7; i++) writer.WriteF32(entity.health[i]);
        writer.WriteU8(entity.alive ? 1 : 0);

        ENetPacket* pkt = enet_packet_create(writer.Data(), writer.Size(), ENET_PACKET_FLAG_RELIABLE);
        if (!pkt) {
            spdlog::error("Failed to create packet ({} bytes)", writer.Size());
            continue;
        }
        enet_peer_send(player.peer, KMP_CHANNEL_RELIABLE_ORDERED, pkt);

        // ES: Envía el equipo de cada ranura ocupada.
        // EN: Send equipment for each non-zero slot
        for (int slot = 0; slot < 14; slot++) {
            if (entity.equipment[slot] != 0) {
                PacketWriter equipWriter;
                equipWriter.WriteHeader(MessageType::S2C_EquipmentUpdate);
                MsgEquipmentUpdate equipMsg{};
                equipMsg.entityId = entity.id;
                equipMsg.slot = static_cast<uint8_t>(slot);
                equipMsg.itemTemplateId = entity.equipment[slot];
                equipWriter.WriteRaw(&equipMsg, sizeof(equipMsg));

                ENetPacket* equipPkt = enet_packet_create(equipWriter.Data(), equipWriter.Size(), ENET_PACKET_FLAG_RELIABLE);
                if (!equipPkt) {
                    spdlog::error("Failed to create packet ({} bytes)", equipWriter.Size());
                    break;
                }
                enet_peer_send(player.peer, KMP_CHANNEL_RELIABLE_UNORDERED, equipPkt);
            }
        }
        sent++;
    }

    if (skippedOrphan > 0 || skippedPosition > 0) {
        spdlog::warn("GameServer: SendWorldSnapshot filtered {}/{} entities (orphan={}, badPos={})",
                     skippedOrphan + skippedPosition, m_entities.size(), skippedOrphan, skippedPosition);
    }
    spdlog::info("GameServer: Sent {} valid entities to player '{}'", sent, player.name);
}

// ES: ── Envío de paquetes ──
// EN: ── Broadcasting ──

// ES: Envía el mismo paquete a todos los peers del host ENet (incluidos peers aún sin handshake).
// EN: Sends the same packet to every peer of the ENet host (including peers without a handshake yet).
void GameServer::Broadcast(const uint8_t* data, size_t len, int channel, uint32_t flags) {
    ENetPacket* pkt = enet_packet_create(data, len, flags);
    if (!pkt) {
        spdlog::error("Failed to create packet ({} bytes)", len);
        return;
    }
    enet_host_broadcast(m_host, channel, pkt);
}

// ES: Envía a todos los jugadores con handshake salvo 'exclude' (un paquete ENet por destinatario).
// EN: Sends to every handshaken player except 'exclude' (one ENet packet per recipient).
void GameServer::BroadcastExcept(PlayerID exclude, const uint8_t* data, size_t len,
                                  int channel, uint32_t flags) {
    int sent = 0;
    for (auto& [id, player] : m_players) {
        if (id == exclude) continue;
        if (!player.peer) continue;
        ENetPacket* pkt = enet_packet_create(data, len, flags);
        if (!pkt) {
            spdlog::error("Failed to create packet ({} bytes)", len);
            continue;
        }
        enet_peer_send(player.peer, channel, pkt);
        sent++;
    }
    spdlog::debug("GameServer: BroadcastExcept(exclude={}) sent to {} peers ({} bytes, ch={})",
                  exclude, sent, len, channel);
}

// ES: Envía a un solo jugador; no hace nada si no existe.
// EN: Sends to a single player; does nothing if it does not exist.
void GameServer::SendTo(PlayerID id, const uint8_t* data, size_t len, int channel, uint32_t flags) {
    auto it = m_players.find(id);
    if (it != m_players.end()) {
        ENetPacket* pkt = enet_packet_create(data, len, flags);
        if (!pkt) {
            spdlog::error("Failed to create packet ({} bytes)", len);
            return;
        }
        enet_peer_send(it->second.peer, channel, pkt);
    }
}

// ES: ── Utilidades ──
// EN: ── Helpers ──

// ES: Jugador de un peer: peer->data guarda el PlayerID como entero. nullptr si aún no hizo handshake.
// EN: Player for a peer: peer->data stores the PlayerID as an integer. nullptr before the handshake.
ConnectedPlayer* GameServer::GetPlayer(ENetPeer* peer) {
    if (!peer || !peer->data) return nullptr;
    PlayerID id = static_cast<PlayerID>(reinterpret_cast<uintptr_t>(peer->data));
    auto it = m_players.find(id);
    return it != m_players.end() ? &it->second : nullptr;
}

// ES: Jugador por ID; nullptr si no está conectado.
// EN: Player by ID; nullptr if not connected.
ConnectedPlayer* GameServer::GetPlayer(PlayerID id) {
    auto it = m_players.find(id);
    return it != m_players.end() ? &it->second : nullptr;
}

// ES: IDs secuenciales desde 1; no se reutilizan durante la vida del proceso.
// EN: Sequential IDs from 1; never reused during the process lifetime.
PlayerID GameServer::NextPlayerId() {
    return m_nextPlayerId++;
}

// ES: ── Comandos de administración (consola) ──
// EN: ── Admin Commands ──

// ES: Expulsa a un jugador desde la consola: mensaje de sistema y desconexión ENet (sus entidades
//     se conservan como en cualquier desconexión).
// EN: Kicks a player from the console: system message and ENet disconnect (its entities are kept as
//     in any disconnect).
void GameServer::KickPlayer(PlayerID id, const std::string& reason) {
    std::lock_guard lock(m_mutex);
    auto* player = GetPlayer(id);
    if (!player) {
        spdlog::warn("GameServer: Player {} not found", id);
        return;
    }

    spdlog::info("GameServer: Kicking player '{}' ({})", player->name, reason);
    BroadcastSystemMessage(player->name + " was kicked: " + reason);
    enet_peer_disconnect(player->peer, 0);
}

// ES: Mensaje de sistema a todos (S2C_SystemMessage con remitente 0) y al log.
// EN: System message to everyone (S2C_SystemMessage with sender 0) and to the log.
void GameServer::BroadcastSystemMessage(const std::string& message) {
    std::lock_guard lock(m_mutex);
    spdlog::info("[System] {}", message);

    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_SystemMessage);
    writer.WriteU32(0); // system
    writer.WriteString(message);
    Broadcast(writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Control de velocidad/pausa global (autoridad del servidor) ──
// El SERVIDOR dicta la velocidad; los clientes la siguen vía TimeSync.
// Estos métodos los llama la consola del server (host = admin).
// EN: ── Global speed/pause control (server authority) ──
//     The SERVER sets the speed; clients follow it through TimeSync.
//     These methods are called by the server console (host = admin).

bool GameServer::SetGameSpeed(float speed) {
    // ES: Validación de rango razonable. 0 = pausa efectiva; tope 10x.
    // EN: Reasonable range check. 0 = effective pause; 10x cap.
    if (speed < 0.f || speed > 10.f) {
        return false;
    }
    std::lock_guard lock(m_mutex);
    m_config.gameSpeed = speed;
    // ES: Si fijamos una velocidad > 0, salimos de pausa automáticamente.
    // EN: Setting a speed > 0 automatically leaves pause.
    if (speed > 0.f) {
        m_serverPaused = false;
    }
    BroadcastTimeSync();  // Empuja inmediatamente la nueva velocidad a todos los clientes
    spdlog::info("GameServer: velocidad global fijada a {:.2f}x (pausado={})",
                 m_config.gameSpeed, m_serverPaused);
    return true;
}

// ES: Pausa global: marca la pausa y envía TimeSync con velocidad 0.
// EN: Global pause: sets the pause flag and sends TimeSync with speed 0.
void GameServer::PauseWorld() {
    std::lock_guard lock(m_mutex);
    m_serverPaused = true;
    BroadcastTimeSync();  // Envía speed=0 a todos los clientes (el formato no cambia)
    spdlog::info("GameServer: mundo PAUSADO por la consola del servidor");
    BroadcastSystemMessage("El servidor ha PAUSADO el mundo.");
}

// ES: Reanuda a la velocidad configurada y la envía con TimeSync.
// EN: Resumes at the configured speed and sends it via TimeSync.
void GameServer::ResumeWorld() {
    std::lock_guard lock(m_mutex);
    m_serverPaused = false;
    BroadcastTimeSync();  // Reanuda a la velocidad global configurada
    spdlog::info("GameServer: mundo REANUDADO a {:.2f}x", m_config.gameSpeed);
    BroadcastSystemMessage("El servidor ha REANUDADO el mundo.");
}

// ES: Carga el mundo guardado al arrancar (hora, clima, entidades sin dueño, jugadores guardados y
//     siguiente ID). No toma m_mutex: se llama una vez desde main antes del bucle de tick.
// EN: Loads the saved world at startup (time, weather, unowned entities, saved players and next
//     ID). Does not take m_mutex: called once from main before the tick loop.
void GameServer::LoadWorld() {
    // ES: Fallback unificado al default real de ServerConfig::savePath ("world.kmpsave").
    // Antes este fallback usaba "kenshi_mp_world.json", distinto al de SaveWorld y al
    // default de config → si alguien vaciaba savePath se cargaba/guardaba en ficheros
    // distintos y se perdía el mundo silenciosamente.
    // EN: Fallback unified with the real ServerConfig::savePath default ("world.kmpsave").
    //     It used to be "kenshi_mp_world.json", different from SaveWorld and the config default, so
    //     emptying savePath made load/save use different files and the world was silently lost.
    std::string savePath = m_config.savePath.empty()
        ? "world.kmpsave" : m_config.savePath;

    float loadedTime = m_timeOfDay;
    int loadedWeather = m_weatherState;
    EntityID loadedNextId = m_nextEntityId;

    if (LoadWorldFromFile(savePath, m_entities, m_savedPlayers, loadedTime, loadedWeather, loadedNextId)) {
        m_timeOfDay = loadedTime;
        m_weatherState = loadedWeather;
        m_nextEntityId = loadedNextId;
        spdlog::info("GameServer: Loaded world from '{}' ({} entities, {} saved players, time={:.2f})",
                     savePath, m_entities.size(), m_savedPlayers.size(), m_timeOfDay);
    } else {
        spdlog::info("GameServer: No saved world at '{}', starting fresh", savePath);
    }
}

// ES: Guarda el mundo (autoguardado, comando 'save' y apagado). El mapa de jugadores combina los
//     desconectados (m_savedPlayers) con la propiedad viva de los conectados.
// EN: Saves the world (autosave, 'save' command and shutdown). The player map merges disconnected
//     players (m_savedPlayers) with the live ownership of connected ones.
void GameServer::SaveWorld() {
    std::lock_guard lock(m_mutex);
    spdlog::info("GameServer: Saving world... ({} entities, {} players)",
                 m_entities.size(), m_players.size());

    // ES: Fallback unificado con LoadWorld y con el default de config ("world.kmpsave").
    // EN: Fallback unified with LoadWorld and the config default ("world.kmpsave").
    std::string savePath = m_config.savePath.empty()
        ? "world.kmpsave" : m_config.savePath;

    // ES: Mapa combinado: jugadores conectados (propiedad viva) + jugadores guardados offline.
    // EN: Build a combined saved-players map: merge currently-connected players
    // (their live entity ownership) with previously-saved offline players.
    auto savedForWrite = m_savedPlayers;
    for (auto& [pid, cp] : m_players) {
        SavedPlayer sp;
        sp.name = cp.name;
        for (auto& [eid, entity] : m_entities) {
            if (entity.owner == pid) sp.entityIds.push_back(eid);
        }
        if (!sp.entityIds.empty()) {
            savedForWrite[cp.name] = std::move(sp);
        }
    }

    if (SaveWorldToFile(savePath, m_entities, savedForWrite, m_timeOfDay, m_weatherState)) {
        spdlog::info("GameServer: World saved to '{}'", savePath);
    } else {
        spdlog::error("GameServer: Failed to save world to '{}'", savePath);
    }
}

// ES: Comandos de consola 'status' y 'players': vuelcan estado y lista de jugadores al log.
// EN: 'status' and 'players' console commands: dump state and player list to the log.
void GameServer::PrintStatus() {
    std::lock_guard lock(m_mutex);
    spdlog::info("=== Server Status ===");
    spdlog::info("Players: {}/{}", m_players.size(), m_config.maxPlayers);
    spdlog::info("Entities: {}", m_entities.size());
    spdlog::info("Tick: {} | Time: {:.2f} | Uptime: {:.0f}s", m_serverTick, m_timeOfDay, m_uptime);
}

void GameServer::PrintPlayers() {
    std::lock_guard lock(m_mutex);
    spdlog::info("=== Connected Players ({}) ===", m_players.size());
    for (auto& [id, player] : m_players) {
        spdlog::info("  [{}] {} - ping: {}ms - zone: ({},{})",
                     id, player.name, player.ping, player.zone.x, player.zone.y);
    }
}

// ES: ── Handlers de inventario ──
// EN: ── Inventory Handlers ──

// ES: C2S_ItemPickup: una entidad propia recoge un objeto -> S2C_InventoryUpdate (acción 0 = añadir) a los demás.
//     El servidor no guarda inventarios: solo valida propiedad y reenvía.
// EN: C2S_ItemPickup: an owned entity picks up an item -> S2C_InventoryUpdate (action 0 = add) to others.
//     The server does not store inventories: it only validates ownership and relays.
void GameServer::HandleItemPickup(ConnectedPlayer& player, PacketReader& reader) {
    MsgItemPickup msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) {
        spdlog::error("GameServer: HandleItemPickup ReadRaw failed for player '{}'", player.name);
        return;
    }

    // ES: Valida que la entidad sea del jugador.
    // EN: Validate entity ownership
    auto it = m_entities.find(msg.entityId);
    if (it == m_entities.end()) {
        spdlog::warn("GameServer: HandleItemPickup entity {} not found (player '{}')",
                     msg.entityId, player.name);
        return;
    }
    if (it->second.owner != player.id) {
        spdlog::warn("GameServer: HandleItemPickup entity {} owner mismatch: entity.owner={} player.id={} (player '{}')",
                     msg.entityId, it->second.owner, player.id, player.name);
        return;
    }

    spdlog::info("GameServer: Player '{}' picked up item {} x{} (entity {})",
                 player.name, msg.itemTemplateId, msg.quantity, msg.entityId);

    // Broadcast inventory update to all other players
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_InventoryUpdate);
    MsgInventoryUpdate update{};
    update.entityId = msg.entityId;
    update.action = 0; // add
    update.itemTemplateId = msg.itemTemplateId;
    update.quantity = msg.quantity;
    writer.WriteRaw(&update, sizeof(update));
    BroadcastExcept(player.id, writer.Data(), writer.Size(),
                   KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: C2S_ItemDrop: una entidad propia suelta un objeto -> S2C_InventoryUpdate (acción 1 = quitar,
//     cantidad 1) a los demás.
// EN: C2S_ItemDrop: an owned entity drops an item -> S2C_InventoryUpdate (action 1 = remove,
//     quantity 1) to others.
void GameServer::HandleItemDrop(ConnectedPlayer& player, PacketReader& reader) {
    MsgItemDrop msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) {
        spdlog::error("GameServer: HandleItemDrop ReadRaw failed for player '{}'", player.name);
        return;
    }

    auto it = m_entities.find(msg.entityId);
    if (it == m_entities.end()) {
        spdlog::warn("GameServer: HandleItemDrop entity {} not found (player '{}')",
                     msg.entityId, player.name);
        return;
    }
    if (it->second.owner != player.id) {
        spdlog::warn("GameServer: HandleItemDrop entity {} owner mismatch: entity.owner={} player.id={} (player '{}')",
                     msg.entityId, it->second.owner, player.id, player.name);
        return;
    }

    spdlog::info("GameServer: Player '{}' dropped item {} at ({:.1f},{:.1f},{:.1f})",
                 player.name, msg.itemTemplateId, msg.posX, msg.posY, msg.posZ);

    // Broadcast to others
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_InventoryUpdate);
    MsgInventoryUpdate update{};
    update.entityId = msg.entityId;
    update.action = 1; // remove
    update.itemTemplateId = msg.itemTemplateId;
    update.quantity = 1;
    writer.WriteRaw(&update, sizeof(update));
    BroadcastExcept(player.id, writer.Data(), writer.Size(),
                   KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: C2S_TradeRequest: compra. Valida comprador propio, cantidad 1-10000, precio >= 0 y que el
//     vendedor exista (0 = tienda NPC). Si falla responde solo al emisor con success = 0; si va bien
//     envía S2C_TradeResult (success = 1) a todos. No hay economía en el servidor (no cobra nada).
// EN: C2S_TradeRequest: purchase. Validates an owned buyer, quantity 1-10000, price >= 0 and that
//     the seller exists (0 = NPC shop). On failure replies only to the sender with success = 0;
//     otherwise sends S2C_TradeResult (success = 1) to everyone. No server-side economy (no charge).
void GameServer::HandleTradeRequest(ConnectedPlayer& player, PacketReader& reader) {
    MsgTradeRequest msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: El comprador debe ser del jugador.
    // EN: Validate buyer ownership
    auto buyerIt = m_entities.find(msg.buyerEntityId);
    if (buyerIt == m_entities.end() || buyerIt->second.owner != player.id) return;

    // ES: Validación básica de la compra.
    // EN: Basic trade validation
    if (msg.quantity <= 0 || msg.quantity > 10000 || msg.price < 0) {
        spdlog::warn("GameServer: Invalid trade from '{}': qty={} price={}",
                     player.name, msg.quantity, msg.price);
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_TradeResult);
        MsgTradeResult result{};
        result.buyerEntityId = msg.buyerEntityId;
        result.itemTemplateId = msg.itemTemplateId;
        result.quantity = msg.quantity;
        result.success = 0; // denied
        writer.WriteRaw(&result, sizeof(result));
        SendTo(player.id, writer.Data(), writer.Size(),
               KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
        return;
    }

    // ES: El vendedor debe existir (0 = tienda NPC, válido).
    // EN: Validate seller entity exists (0 = NPC shop, which is OK)
    if (msg.sellerEntityId != 0) {
        auto sellerIt = m_entities.find(msg.sellerEntityId);
        if (sellerIt == m_entities.end()) {
            spdlog::warn("GameServer: Trade seller entity {} not found", msg.sellerEntityId);
            PacketWriter writer;
            writer.WriteHeader(MessageType::S2C_TradeResult);
            MsgTradeResult result{};
            result.buyerEntityId = msg.buyerEntityId;
            result.itemTemplateId = msg.itemTemplateId;
            result.quantity = msg.quantity;
            result.success = 0;
            writer.WriteRaw(&result, sizeof(result));
            SendTo(player.id, writer.Data(), writer.Size(),
                   KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
            return;
        }
    }

    spdlog::info("GameServer: Trade from '{}': buyer={} seller={} item={} qty={} price={}",
                 player.name, msg.buyerEntityId, msg.sellerEntityId,
                 msg.itemTemplateId, msg.quantity, msg.price);

    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_TradeResult);
    MsgTradeResult result{};
    result.buyerEntityId = msg.buyerEntityId;
    result.itemTemplateId = msg.itemTemplateId;
    result.quantity = msg.quantity;
    result.success = 1;
    writer.WriteRaw(&result, sizeof(result));
    Broadcast(writer.Data(), writer.Size(),
             KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Handlers de escuadras ──
// EN: ── Squad Handlers ──

// ES: C2S_SquadCreate: [u32 entidad creadora][string nombre]. Asigna un ID de escuadra del servidor
//     (espacio propio desde 0x80000000) y avisa a todos con S2C_SquadCreated.
// EN: C2S_SquadCreate: [u32 creator entity][string name]. Assigns a server squad ID (own range from
//     0x80000000) and notifies everyone with S2C_SquadCreated.
void GameServer::HandleSquadCreate(ConnectedPlayer& player, PacketReader& reader) {
    uint32_t creatorEntityId;
    if (!reader.ReadU32(creatorEntityId)) return;
    std::string squadName;
    if (!reader.ReadString(squadName)) return;

    // ES: La entidad creadora debe ser del jugador.
    // EN: Validate creator ownership
    auto it = m_entities.find(creatorEntityId);
    if (it == m_entities.end() || it->second.owner != player.id) return;

    // ES: ID de escuadra del servidor (espacio separado para no chocar con IDs de entidad).
    // EN: Assign server-side squad ID (separate ID space to avoid entity collisions)
    uint32_t squadNetId = m_nextSquadId++;

    spdlog::info("GameServer: Player '{}' created squad '{}' (netId={})",
                 player.name, squadName, squadNetId);

    // ES: Aviso a todos los jugadores.
    // EN: Broadcast to all players
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_SquadCreated);
    writer.WriteU32(creatorEntityId);
    writer.WriteU32(squadNetId);
    writer.WriteString(squadName);
    Broadcast(writer.Data(), writer.Size(),
             KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: C2S_SquadAddMember: añadir/quitar (action 0/1) un miembro propio a una escuadra; se reenvía a los demás.
// EN: C2S_SquadAddMember: add/remove (action 0/1) an owned member to a squad; relayed to others.
void GameServer::HandleSquadAddMember(ConnectedPlayer& player, PacketReader& reader) {
    MsgSquadMemberUpdate msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: El miembro debe ser del jugador.
    // EN: Validate member ownership
    auto it = m_entities.find(msg.memberEntityId);
    if (it == m_entities.end() || it->second.owner != player.id) return;

    spdlog::info("GameServer: Player '{}' {} member {} to/from squad {}",
                 player.name, msg.action == 0 ? "added" : "removed",
                 msg.memberEntityId, msg.squadNetId);

    // ES: Reenvía al resto de jugadores.
    // EN: Broadcast to all other players
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_SquadMemberUpdate);
    writer.WriteRaw(&msg, sizeof(msg));
    BroadcastExcept(player.id, writer.Data(), writer.Size(),
                   KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Handlers de facciones ──
// EN: ── Faction Handlers ──

// ES: C2S_FactionRelation: cambio de relación entre dos facciones (-100..100). Si hay entidad causante
//     debe ser del jugador. Se envía a TODOS (emisor incluido) para mantener la coherencia.
// EN: C2S_FactionRelation: relation change between two factions (-100..100). If there is a causer
//     entity it must be the player's. Sent to EVERYONE (sender included) for consistency.
void GameServer::HandleFactionRelation(ConnectedPlayer& player, PacketReader& reader) {
    MsgFactionRelation msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: Relación dentro de -100..100.
    // EN: Validate relation bounds
    if (msg.relation < -100.f || msg.relation > 100.f) {
        spdlog::warn("GameServer: Player '{}' sent invalid relation value {:.1f}", player.name, msg.relation);
        return;
    }

    // ES: La entidad causante (si no es 0) debe ser del jugador.
    // EN: Validate causer entity belongs to this player (if non-zero)
    if (msg.causerEntityId != 0) {
        auto it = m_entities.find(msg.causerEntityId);
        if (it == m_entities.end() || it->second.owner != player.id) {
            spdlog::warn("GameServer: Player '{}' sent faction relation with invalid causer {}",
                         player.name, msg.causerEntityId);
            return;
        }
    }

    spdlog::info("GameServer: Faction relation change from '{}': faction {} <-> {} = {:.1f}",
                 player.name, msg.factionIdA, msg.factionIdB, msg.relation);

    // ES: Aviso a TODOS (emisor incluido) para asegurar coherencia.
    // EN: Broadcast to ALL players (including sender) to ensure consistency
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_FactionRelation);
    writer.WriteRaw(&msg, sizeof(msg));
    Broadcast(writer.Data(), writer.Size(),
             KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Handlers de edificios ──
// EN: ── Building Handlers ──

// ES: C2S_BuildDismantle: desmontar un edificio (solo su dueño o el host). Se borra del servidor y
//     se avisa a los demás con S2C_BuildDestroyed (motivo 2 = desmontado).
// EN: C2S_BuildDismantle: dismantle a building (owner or host only). Removed server-side and the
//     others get S2C_BuildDestroyed (reason 2 = dismantled).
void GameServer::HandleBuildDismantle(ConnectedPlayer& player, PacketReader& reader) {
    MsgBuildDismantle msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) {
        spdlog::error("GameServer: HandleBuildDismantle ReadRaw failed for player '{}'", player.name);
        return;
    }

    // ES: Busca el edificio.
    // EN: Find building
    auto it = m_entities.find(msg.buildingId);
    if (it == m_entities.end()) {
        spdlog::warn("GameServer: HandleBuildDismantle building {} not found (player '{}', m_entities size={})",
                     msg.buildingId, player.name, m_entities.size());
        return;
    }

    // ES: Solo el dueño del edificio (o el host) puede desmontarlo.
    // EN: Ownership check: only the building owner (or host) can dismantle
    if (it->second.owner != player.id && player.id != m_hostPlayerId) {
        spdlog::warn("GameServer: Player '{}' tried to dismantle building {} owned by player {}",
                     player.name, msg.buildingId, it->second.owner);
        return;
    }

    spdlog::info("GameServer: Player '{}' dismantled building {} (owner={})",
                 player.name, msg.buildingId, it->second.owner);

    // ES: Quita el edificio del estado del servidor.
    // EN: Remove building from server state
    m_entities.erase(it);

    // ES: Aviso de destrucción (en realidad a todos menos al emisor).
    // EN: Broadcast destruction to all
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_BuildDestroyed);
    writer.WriteU32(msg.buildingId);
    writer.WriteU8(2); // reason: dismantled
    BroadcastExcept(player.id, writer.Data(), writer.Size(),
                    KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: C2S_BuildRepair: suma progreso de construcción/reparación (0 < amount <= 1, total acotado a
//     0..1) a un edificio del jugador o del host y envía S2C_BuildProgress a los demás.
// EN: C2S_BuildRepair: adds build/repair progress (0 < amount <= 1, total clamped to 0..1) to a
//     building owned by the player (or host) and sends S2C_BuildProgress to the others.
void GameServer::HandleBuildRepair(ConnectedPlayer& player, PacketReader& reader) {
    MsgBuildRepair msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    auto it = m_entities.find(msg.buildingId);
    if (it == m_entities.end()) return;

    // ES: Solo el dueño del edificio (o el host) puede repararlo.
    // EN: Ownership check: only the building owner (or host) can repair
    if (it->second.owner != player.id && player.id != m_hostPlayerId) {
        spdlog::warn("GameServer: Player '{}' tried to repair building {} owned by player {}",
                     player.name, msg.buildingId, it->second.owner);
        return;
    }

    // ES: Cantidad válida (rechaza negativos, NaN y valores enormes).
    // EN: Validate repair amount (reject negative, NaN, or unreasonably large values)
    if (msg.amount <= 0.f || std::isnan(msg.amount) || msg.amount > 1.f) return;

    // ES: Actualiza el progreso en el servidor.
    // EN: Update server-side building progress
    it->second.buildProgress = std::clamp(it->second.buildProgress + msg.amount, 0.f, 1.f);

    spdlog::debug("GameServer: Building {} repair +{:.2f} -> {:.2f} by '{}'",
                  msg.buildingId, msg.amount, it->second.buildProgress, player.name);

    // ES: Aviso del progreso.
    // EN: Broadcast progress update
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_BuildProgress);
    MsgBuildProgress progress{};
    progress.entityId = msg.buildingId;
    progress.progress = it->second.buildProgress;
    writer.WriteRaw(&progress, sizeof(progress));
    BroadcastExcept(player.id, writer.Data(), writer.Size(),
                   KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Handler de postura de combate ──
// EN: ── Combat Stance Handler ──

// ES: C2S_CombatStance: postura de combate (0-3) de una entidad propia. Se reenvía reutilizando el
//     tipo S2C_CombatBlock porque no hay un tipo S2C propio para la postura.
// EN: C2S_CombatStance: combat stance (0-3) of an owned entity. Relayed reusing the S2C_CombatBlock
//     type because there is no dedicated S2C stance type.
void GameServer::HandleCombatStance(ConnectedPlayer& player, PacketReader& reader) {
    MsgCombatStance msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: Valida que la entidad sea del jugador.
    // EN: Validate entity ownership
    auto it = m_entities.find(msg.entityId);
    if (it == m_entities.end() || it->second.owner != player.id) return;

    // ES: Postura válida (0-3).
    // EN: Validate stance value
    if (msg.stance > 3) return;

    spdlog::debug("GameServer: Player '{}' set entity {} stance to {}", player.name, msg.entityId, msg.stance);

    // ES: Reenvía al resto de jugadores.
    // EN: Broadcast to other players
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_CombatBlock); // Reuse CombatBlock for stance sync (no dedicated S2C type)
    writer.WriteRaw(&msg, sizeof(msg));
    BroadcastExcept(player.id, writer.Data(), writer.Size(),
                   KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Handler de KO ──
// EN: ── Combat KO Handler ──

// ES: C2S_CombatKO: un cliente informa de un KO (u32 entidad, u32 atacante, u8 motivo, f32 vida del
//     pecho). Lo acepta del dueño de la víctima o del dueño del atacante; guarda la vida en health[0]
//     y reenvía S2C_CombatKO a TODOS (el campo bodyPart lleva en realidad el motivo).
// EN: C2S_CombatKO: a client reports a KO (u32 entity, u32 attacker, u8 reason, f32 chest health).
//     Accepted from the victim owner or the attacker owner; stores the health in health[0] and sends
//     S2C_CombatKO to EVERYONE (the bodyPart field actually carries the reason).
void GameServer::HandleCombatKO(ConnectedPlayer& player, PacketReader& reader) {
    // ES: El cliente envía: entityId(u32), attackerId(u32), reason(u8), chestHealth(f32).
    // EN: Client sends: entityId(u32), attackerId(u32), reason(u8), chestHealth(f32)
    uint32_t entityId = 0, attackerId = 0;
    uint8_t reason = 0;
    float chestHealth = 0.f;
    if (!reader.ReadU32(entityId) || !reader.ReadU32(attackerId) ||
        !reader.ReadU8(reason) || !reader.ReadF32(chestHealth)) return;

    // ES: La entidad debe existir y estar viva (no hay KO de muertos).
    // EN: Validate entity exists and is alive (reject KO on dead entities)
    auto it = m_entities.find(entityId);
    if (it == m_entities.end() || !it->second.alive) return;

    // ES: Combate entre jugadores: se acepta el KO del dueño de la víctima o del dueño del atacante.
    // EN: Cross-player combat: accept KO reports from entity owner or attacker owner
    bool victimIsReporter = (it->second.owner == player.id);
    bool attackerIsReporter = false;
    if (attackerId != 0) {
        auto attackerIt = m_entities.find(attackerId);
        attackerIsReporter = (attackerIt != m_entities.end() && attackerIt->second.owner == player.id);
    }
    if (!victimIsReporter && !attackerIsReporter) return;

    // ES: H5: validar la salud recibida antes de guardarla y retransmitirla. Mismo criterio
    // que HandlePositionUpdate: rechazar NaN/inf (un cliente malicioso podría envenenar
    // el estado de otros clientes) y acotar a un rango plausible con std::clamp.
    // EN: H5: validate the received health before storing and relaying it. Same criteria as
    //     HandlePositionUpdate: reject NaN/inf (a malicious client could poison other clients state)
    //     and clamp to a plausible range with std::clamp.
    if (std::isnan(chestHealth) || std::isinf(chestHealth)) {
        spdlog::warn("GameServer: KO de '{}' con chestHealth invalido (NaN/inf) para entidad {}",
                     player.name, entityId);
        return;
    }
    chestHealth = std::clamp(chestHealth, -1000.0f, 1000.0f);

    spdlog::info("GameServer: Player '{}' reports entity {} KO (attacker={}, reason={}, health={:.1f})",
                 player.name, entityId, attackerId, reason, chestHealth);

    // ES: Actualiza la vida en el servidor. 'reason' es la causa del KO (0 = pérdida de sangre,
    //     1 = traumatismo craneal, 2 = otra), NO un índice de parte; la vida siempre es la del pecho,
    //     por eso se escribe en health[0]. OJO: en kmp/types.h BodyPart::Chest vale 1 y el índice 0
    //     es Head, así que esta línea escribe la vida del pecho en la cabeza (probable fallo).
    //     NOTE: in kmp/types.h BodyPart::Chest is 1 and index 0 is Head, so this line stores the
    //     chest health into the head slot (probable bug).
    // EN: Update server-side health — 'reason' is the KO cause (0=blood loss,
    // 1=head trauma, 2=other), NOT a body part index. The health value is
    // always chest health, so write to health[0] (chest).
    it->second.health[0] = chestHealth;

    // ES: Aviso del KO a TODOS (emisor incluido, por coherencia).
    // EN: Broadcast KO to ALL players (including reporter for consistency)
    MsgCombatKO koMsg{};
    koMsg.entityId = entityId;
    koMsg.attackerId = attackerId;
    koMsg.bodyPart = reason;
    koMsg.resultHealth = chestHealth;

    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_CombatKO);
    writer.WriteRaw(&koMsg, sizeof(koMsg));
    Broadcast(writer.Data(), writer.Size(),
                   KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Handler de muerte ──
// EN: ── Combat Death Handler ──

// ES: C2S_CombatDeath: un cliente informa de una muerte. Ignora duplicados (ya muerta), la acepta del
//     dueño de la víctima o del dueño del asesino, marca alive = false y la reenvía a TODOS.
// EN: C2S_CombatDeath: a client reports a death. Ignores duplicates (already dead), accepts it from
//     the victim owner or the killer owner, sets alive = false and relays it to EVERYONE.
void GameServer::HandleCombatDeath(ConnectedPlayer& player, PacketReader& reader) {
    MsgCombatDeath msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: La entidad debe existir.
    // EN: Validate entity exists
    auto it = m_entities.find(msg.entityId);
    if (it == m_entities.end()) return;

    // ES: Ignora informes duplicados: ambos jugadores pueden informar de la misma muerte.
    // EN: Reject duplicate death reports — both players may report the same kill
    if (!it->second.alive) {
        spdlog::debug("GameServer: Ignoring duplicate death report for entity {} from '{}'",
                      msg.entityId, player.name);
        return;
    }

    // ES: Combate entre jugadores: se acepta del dueño de la víctima o del dueño del asesino. Si el
    //     jugador A mata a un personaje de B, A informa con killerId apuntando a su propia entidad.
    // EN: Cross-player combat: accept death reports from either the entity owner
    // or the killer's owner. When player A kills player B's character, player A
    // reports the death with killerId pointing to their own entity.
    bool victimIsReporter = (it->second.owner == player.id);
    bool killerIsReporter = false;
    if (msg.killerId != 0) {
        auto killerIt = m_entities.find(msg.killerId);
        killerIsReporter = (killerIt != m_entities.end() && killerIt->second.owner == player.id);
    }
    if (!victimIsReporter && !killerIsReporter) return;

    // ES: Marca la entidad como muerta para rechazar futuros ataques.
    // EN: Mark entity as dead on the server so future attacks are rejected
    it->second.alive = false;

    spdlog::info("GameServer: Player '{}' reports entity {} death (killer={}, reporter={})",
                 player.name, msg.entityId, msg.killerId,
                 victimIsReporter ? "victim-owner" : "killer-owner");

    // ES: Aviso de la muerte a TODOS (emisor incluido) para sincronizar.
    // EN: Broadcast death to ALL players (including reporter for authoritative sync)
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_CombatDeath);
    writer.WriteRaw(&msg, sizeof(msg));
    Broadcast(writer.Data(), writer.Size(),
                   KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Handler de vida de miembros ──
// EN: ── Limb Health Handler ──

// ES: C2S_LimbHealth: vida de los 7 miembros de una entidad propia. Rechaza NaN/inf, acota a
//     -1000..1000, la guarda en limbHealth y la reenvía (ya saneada) a los demás.
// EN: C2S_LimbHealth: health of the 7 limbs of an owned entity. Rejects NaN/inf, clamps to
//     -1000..1000, stores it in limbHealth and relays it (sanitized) to the others.
void GameServer::HandleLimbHealth(ConnectedPlayer& player, PacketReader& reader) {
    MsgLimbHealth msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: Valida que la entidad sea del jugador.
    // EN: Validate entity ownership
    auto it = m_entities.find(msg.entityId);
    if (it == m_entities.end() || it->second.owner != player.id) return;

    // ES: H5: rechazar el paquete si cualquier valor de salud de miembro es NaN/inf
    // (mismo criterio que HandlePositionUpdate). Evita propagar valores corruptos
    // del cliente al estado del servidor y al resto de clientes.
    // EN: H5: reject the packet if any limb health value is NaN/inf (same criteria as
    //     HandlePositionUpdate). Avoids spreading corrupt client values to the server state and
    //     to the other clients.
    for (int i = 0; i < 7; i++) {
        if (std::isnan(msg.health[i]) || std::isinf(msg.health[i])) {
            spdlog::warn("GameServer: LimbHealth de '{}' con valor invalido (NaN/inf) para entidad {}",
                         player.name, msg.entityId);
            return;
        }
    }

    // ES: Guarda la vida de miembros en la entidad del servidor, acotando cada valor a un rango
    //     plausible con std::clamp y saneando también msg.health[] (se reenvía más abajo).
    // EN: Store limb health values on the server entity, clamping each value to a plausible range
    //     with std::clamp and also sanitizing msg.health[] (it is relayed below).
    for (int i = 0; i < 7; i++) {
        float h = std::clamp(msg.health[i], -1000.0f, 1000.0f);
        msg.health[i] = h;
        it->second.limbHealth[i] = h;
    }

    spdlog::debug("GameServer: Player '{}' limb health for entity {} (chest={:.1f})",
                  player.name, msg.entityId, msg.health[static_cast<int>(BodyPart::Chest)]);

    // ES: Reenvía al resto de jugadores.
    // EN: Broadcast to other players
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_LimbHealth);
    writer.WriteRaw(&msg, sizeof(msg));
    BroadcastExcept(player.id, writer.Data(), writer.Size(),
                    KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Handler de efectos de estado ──
// EN: ── Status Effect Handler ──

// ES: C2S_StatusEffect: activa/desactiva un efecto de estado (hasta StatusEffect_Bandaged) de una
//     entidad propia; se guarda y se reenvía a los demás.
// EN: C2S_StatusEffect: turns a status effect (up to StatusEffect_Bandaged) on/off for an owned
//     entity; stored and relayed to the others.
void GameServer::HandleStatusEffect(ConnectedPlayer& player, PacketReader& reader) {
    MsgStatusEffect msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: Valida que la entidad sea del jugador.
    // EN: Validate entity ownership
    auto it = m_entities.find(msg.entityId);
    if (it == m_entities.end() || it->second.owner != player.id) return;

    // ES: Tipo de efecto dentro de rango.
    // EN: Validate effect type range
    if (msg.effectType > StatusEffect_Bandaged) return;

    // ES: Guarda el efecto en la entidad del servidor.
    // EN: Store status effect on server entity
    it->second.statusEffects[msg.effectType] = msg.active;

    spdlog::debug("GameServer: Player '{}' entity {} status effect {} = {}",
                  player.name, msg.entityId, msg.effectType, msg.active);

    // ES: Reenvía al resto de jugadores.
    // EN: Broadcast to other players
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_StatusEffect);
    writer.WriteRaw(&msg, sizeof(msg));
    BroadcastExcept(player.id, writer.Data(), writer.Size(),
                    KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Handler de transferencia de objetos ──
// EN: ── Item Transfer Handler ──

// ES: C2S_ItemTransfer: mover objetos de una entidad propia a otra existente (cantidad 1-10000).
//     Se traduce en dos S2C_InventoryUpdate a los demás: quitar del origen y añadir al destino.
// EN: C2S_ItemTransfer: move items from an owned entity to another existing one (quantity 1-10000).
//     Becomes two S2C_InventoryUpdate for the others: remove from source and add to destination.
void GameServer::HandleItemTransfer(ConnectedPlayer& player, PacketReader& reader) {
    MsgItemTransfer msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: El jugador debe ser dueño del origen.
    // EN: Validate: player must own the source entity
    auto srcIt = m_entities.find(msg.sourceEntityId);
    if (srcIt == m_entities.end() || srcIt->second.owner != player.id) {
        spdlog::warn("GameServer: Player '{}' tried to transfer from entity {} they don't own",
                     player.name, msg.sourceEntityId);
        return;
    }

    // ES: El destino debe existir.
    // EN: Validate: destination entity must exist
    auto destIt = m_entities.find(msg.destEntityId);
    if (destIt == m_entities.end()) {
        spdlog::warn("GameServer: Transfer dest entity {} not found", msg.destEntityId);
        return;
    }

    // ES: Cantidad válida.
    // EN: Validate quantity
    if (msg.quantity <= 0 || msg.quantity > 10000) return;

    spdlog::info("GameServer: Player '{}' transferred {}x item {} from entity {} to {}",
                 player.name, msg.quantity, msg.itemTemplateId,
                 msg.sourceEntityId, msg.destEntityId);

    // ES: Avisos de inventario (a todos menos al emisor): el origen pierde el objeto...
    // EN: Broadcast inventory updates to affected players
    // Source loses item
    {
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_InventoryUpdate);
        MsgInventoryUpdate update{};
        update.entityId = msg.sourceEntityId;
        update.action = 1; // remove
        update.itemTemplateId = msg.itemTemplateId;
        update.quantity = msg.quantity;
        writer.WriteRaw(&update, sizeof(update));
        BroadcastExcept(player.id, writer.Data(), writer.Size(),
                       KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
    }

    // ES: ...y el destino lo gana.
    // EN: Destination gains item
    {
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_InventoryUpdate);
        MsgInventoryUpdate update{};
        update.entityId = msg.destEntityId;
        update.action = 0; // add
        update.itemTemplateId = msg.itemTemplateId;
        update.quantity = msg.quantity;
        writer.WriteRaw(&update, sizeof(update));
        BroadcastExcept(player.id, writer.Data(), writer.Size(),
                       KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
    }
}

// ES: ── Handler de puertas ──
// EN: ── Door Interaction Handler ──

// ES: C2S_DoorInteract: una entidad propia abre/cierra/bloquea/desbloquea (0-3) una puerta o portón.
//     Se envía S2C_DoorState a TODOS; la acción se usa directamente como estado.
// EN: C2S_DoorInteract: an owned entity opens/closes/locks/unlocks (0-3) a door or gate.
//     S2C_DoorState is sent to EVERYONE; the action is used directly as the state.
void GameServer::HandleDoorInteract(ConnectedPlayer& player, PacketReader& reader) {
    MsgDoorInteract msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: El actor debe ser del jugador.
    // EN: Validate: actor must be owned by player
    auto actorIt = m_entities.find(msg.actorEntityId);
    if (actorIt == m_entities.end() || actorIt->second.owner != player.id) return;

    // ES: El edificio (puerta) debe existir.
    // EN: Validate: building entity must exist
    auto buildingIt = m_entities.find(msg.entityId);
    if (buildingIt == m_entities.end()) return;

    // ES: Acción válida (0-3).
    // EN: Validate action
    if (msg.action > 3) return;

    const char* actionNames[] = {"open", "close", "lock", "unlock"};
    spdlog::info("GameServer: Player '{}' {} door/gate {}",
                 player.name, actionNames[msg.action], msg.entityId);

    // ES: Aviso del estado de la puerta a todos.
    // EN: Broadcast door state to all players
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_DoorState);
    MsgDoorState state{};
    state.entityId = msg.entityId;
    state.state = msg.action; // action maps directly to state enum
    writer.WriteRaw(&state, sizeof(state));
    Broadcast(writer.Data(), writer.Size(),
             KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Handler de comandos de admin ── (OJO: justo debajo va primero HandleLobbyReady, que no es
//     un comando de admin; HandleAdminCommand viene después).
// EN: ── Admin Command Handler ──

// ES: C2S_LobbyReady: el jugador pulsa "listo" en el lobby previo a la partida. Cuando TODOS los
//     conectados lo están, se envía S2C_LobbyStart con el número de jugadores. No confundir con
//     HandlePlayerReady (partida ya cargada).
// EN: C2S_LobbyReady: the player presses "ready" in the pre-game lobby. When ALL connected players
//     are ready, S2C_LobbyStart is sent with the player count. Not to be confused with
//     HandlePlayerReady (game already loaded).
void GameServer::HandleLobbyReady(ConnectedPlayer& player, PacketReader& reader) {
    player.lobbyReady = true;
    spdlog::info("GameServer: Player '{}' (slot {}) is READY", player.name, player.id);

    // ES: Avisa a todos de que este jugador está listo.
    // EN: Broadcast to all clients that this player is ready
    BroadcastSystemMessage(player.name + " is ready!");

    // ES: Comprueba si TODOS los conectados están listos.
    // EN: Check if ALL connected players are ready
    bool allReady = true;
    int readyCount = 0;
    for (const auto& [id, p] : m_players) {
        if (p.lobbyReady) {
            readyCount++;
        } else {
            allReady = false;
        }
    }

    spdlog::info("GameServer: {}/{} players ready", readyCount, m_players.size());

    if (allReady && !m_players.empty()) {
        spdlog::info("GameServer: ALL PLAYERS READY — sending LobbyStart!");
        BroadcastSystemMessage("All players ready — starting game!");

        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_LobbyStart);
        writer.WriteU8(static_cast<uint8_t>(m_players.size()));
        Broadcast(writer.Data(), writer.Size(),
                  KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
    }
}

// ES: C2S_AdminCommand: comandos remotos del host (solo el host; al resto se le responde "permiso
//     denegado"). Casos: 0 = expulsar, 1 = banear (nombre + IP, persistente), 2 = hora (0-1),
//     3 = clima (0-4), 4 = anuncio, 5 = velocidad (0.1-10). Siempre responde con S2C_AdminResponse.
//     OJO: responde success = 1 aunque el comando falle (el texto lo dice); el caso 5 no quita la
//     pausa, a diferencia de SetGameSpeed.
// EN: C2S_AdminCommand: remote host commands (host only; others get "permission denied").
//     Cases: 0 = kick, 1 = ban (name + IP, persisted), 2 = time (0-1), 3 = weather (0-4),
//     4 = announce, 5 = speed (0.1-10). Always replies with S2C_AdminResponse.
//     NOTE: it replies success = 1 even if the command fails (the text says so); case 5 does not
//     clear the pause, unlike SetGameSpeed.
void GameServer::HandleAdminCommand(ConnectedPlayer& player, PacketReader& reader) {
    MsgAdminCommand msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    // ES: Solo el host puede usar comandos de admin.
    // EN: Only the host can issue admin commands
    if (player.id != m_hostPlayerId) {
        spdlog::warn("GameServer: Non-host player '{}' tried admin command type {}",
                     player.name, msg.commandType);
        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_AdminResponse);
        MsgAdminResponse resp{};
        resp.success = 0;
        snprintf(resp.responseText, sizeof(resp.responseText), "Permission denied: only the host can use admin commands.");
        writer.WriteRaw(&resp, sizeof(resp));
        SendTo(player.id, writer.Data(), writer.Size(),
               KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
        return;
    }

    std::string responseText;

    switch (msg.commandType) {
    case 0: { // Kick
        auto* target = GetPlayer(msg.targetPlayerId);
        if (target && target->id != m_hostPlayerId) {
            // ES: H6: construir el string acotando la lectura a sizeof(textParam) con strnlen,
            // por si el cliente no null-terminó el buffer (evita over-read más allá de 128 bytes).
            // EN: H6: build the string bounding the read to sizeof(textParam) with strnlen, in case the client
            //     did not null-terminate the buffer (avoids over-reading past 128 bytes).
            std::string reason = msg.textParam[0]
                ? std::string(msg.textParam, strnlen(msg.textParam, sizeof(msg.textParam)))
                : std::string("Kicked by host");
            spdlog::info("GameServer: Host kicked player '{}': {}", target->name, reason);
            BroadcastSystemMessage(target->name + " was kicked: " + reason);

            // ES: Envía un reject (código 2) al expulsado y lo desconecta tras enviarlo.
            // EN: Send reject to kicked player
            PacketWriter kickWriter;
            kickWriter.WriteHeader(MessageType::S2C_HandshakeReject);
            MsgHandshakeReject reject{};
            reject.reasonCode = 2; // kicked
            strncpy(reject.reasonText, reason.c_str(), sizeof(reject.reasonText) - 1);
            kickWriter.WriteRaw(&reject, sizeof(reject));
            ENetPacket* kickPkt = enet_packet_create(kickWriter.Data(), kickWriter.Size(), ENET_PACKET_FLAG_RELIABLE);
            if (!kickPkt) {
                spdlog::error("Failed to create packet ({} bytes)", kickWriter.Size());
                enet_peer_disconnect_later(target->peer, 0);
            } else {
                enet_peer_send(target->peer, KMP_CHANNEL_RELIABLE_ORDERED, kickPkt);
                enet_peer_disconnect_later(target->peer, 0);
            }

            responseText = "Kicked " + target->name;
        } else {
            responseText = "Player not found or cannot kick host.";
        }
        break;
    }
    case 1: { // Ban
        auto* target = GetPlayer(msg.targetPlayerId);
        if (target && target->id != m_hostPlayerId) {
            // ES: H6: acotar la lectura con strnlen por si el buffer no viene null-terminado.
            // EN: H6: bound the read with strnlen in case the buffer is not null-terminated.
            std::string reason = msg.textParam[0]
                ? std::string(msg.textParam, strnlen(msg.textParam, sizeof(msg.textParam)))
                : std::string("Baneado por el host");
            std::string bannedName = target->name;
            std::string bannedIP   = AddressToIPString(target->peer->address);

            // ES: Registrar baneo por nombre y por IP (la IP es frágil con NAT/dinámica,
            // por eso se combina con el nombre).
            // EN: Register the ban by name and by IP (the IP is fragile with NAT/dynamic addresses, which
            //     is why it is combined with the name).
            m_bannedNames.insert(bannedName);
            if (!bannedIP.empty()) m_bannedIPs.insert(bannedIP);
            SaveBans(); // Persistir inmediatamente

            spdlog::info("GameServer: Host baneo a '{}' (IP {}): {}", bannedName, bannedIP, reason);
            BroadcastSystemMessage(bannedName + " fue baneado: " + reason);

            // ES: No preservar sus entidades como reconectables: purgar su registro guardado
            // y dejar sus entidades vivas como server-owned (owner=0) sin mapping de reclamo.
            // EN: Do not keep its entities as reconnectable: purge its saved record and leave its live
            //     entities as server-owned (owner=0) with no reclaim mapping.
            m_savedPlayers.erase(bannedName);

            // ES: Enviar reject y desconectar.
            // EN: Send the reject and disconnect.
            PacketWriter banWriter;
            banWriter.WriteHeader(MessageType::S2C_HandshakeReject);
            MsgHandshakeReject reject{};
            reject.reasonCode = 2; // banned/kicked
            strncpy(reject.reasonText, reason.c_str(), sizeof(reject.reasonText) - 1);
            banWriter.WriteRaw(&reject, sizeof(reject));
            ENetPacket* banPkt = enet_packet_create(banWriter.Data(), banWriter.Size(), ENET_PACKET_FLAG_RELIABLE);
            if (!banPkt) {
                spdlog::error("Failed to create packet ({} bytes)", banWriter.Size());
                enet_peer_disconnect_later(target->peer, 0);
            } else {
                enet_peer_send(target->peer, KMP_CHANNEL_RELIABLE_ORDERED, banPkt);
                enet_peer_disconnect_later(target->peer, 0);
            }

            responseText = "Baneado " + bannedName;
        } else {
            responseText = "Jugador no encontrado o no se puede banear al host.";
        }
        break;
    }
    case 2: { // Set time
        float newTime = msg.floatParam;
        if (newTime >= 0.f && newTime < 1.f) {
            m_timeOfDay = newTime;
            BroadcastTimeSync();
            char buf[64];
            snprintf(buf, sizeof(buf), "Time set to %.2f", newTime);
            responseText = buf;
        } else {
            responseText = "Invalid time (0.0-1.0)";
        }
        break;
    }
    case 3: { // Set weather
        int weather = static_cast<int>(msg.floatParam);
        if (weather >= 0 && weather <= 4) {
            m_weatherState = weather;
            BroadcastTimeSync(); // Time sync includes weather
            const char* names[] = {"Clear", "Cloudy", "Dust Storm", "Rain", "Acid Rain"};
            responseText = std::string("Weather set to ") + names[weather];
        } else {
            responseText = "Invalid weather (0-4)";
        }
        break;
    }
    case 4: { // Announce
        // ES: H6: acotar la lectura con strnlen por si el buffer no viene null-terminado.
        // EN: H6: bound the read with strnlen in case the buffer is not null-terminated.
        std::string announcement(msg.textParam, strnlen(msg.textParam, sizeof(msg.textParam)));
        if (!announcement.empty()) {
            BroadcastSystemMessage("[HOST] " + announcement);
            responseText = "Announced.";
        } else {
            responseText = "Empty announcement.";
        }
        break;
    }
    case 5: { // Set game speed
        float newSpeed = msg.floatParam;
        if (newSpeed >= 0.1f && newSpeed <= 10.0f) {
            m_config.gameSpeed = newSpeed;
            BroadcastTimeSync();  // Immediately push new speed to all clients
            char buf[64];
            snprintf(buf, sizeof(buf), "Game speed set to %.2fx", newSpeed);
            responseText = buf;
            spdlog::info("GameServer: Host changed gameSpeed to {:.2f}x", newSpeed);
        } else {
            responseText = "Invalid speed (must be 0.1-10.0)";
        }
        break;
    }
    default:
        responseText = "Unknown admin command.";
        break;
    }

    // ES: Respuesta al host con el resultado en texto.
    // EN: Send response
    PacketWriter writer;
    writer.WriteHeader(MessageType::S2C_AdminResponse);
    MsgAdminResponse resp{};
    resp.success = 1;
    strncpy(resp.responseText, responseText.c_str(), sizeof(resp.responseText) - 1);
    writer.WriteRaw(&resp, sizeof(resp));
    SendTo(player.id, writer.Data(), writer.Size(),
           KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: ── Registro en el master server ──
//     Opcional y aislado: usa un host ENet aparte (m_masterHost) y sus fallos solo se registran,
//     nunca paran el servidor ni el juego por IP directa.
// EN: ── Master Server Registration ──

// ES: Crea el host ENet del master (1 peer, 1 canal) e inicia la conexión a masterServer:masterPort.
//     No hace nada si enableMasterServer es false o masterServer está vacío.
// EN: Creates the master ENet host (1 peer, 1 channel) and starts connecting to
//     masterServer:masterPort. Does nothing if enableMasterServer is false or masterServer is empty.
void GameServer::ConnectToMaster() {
    // ES: El registro al master server es opcional. Para partidas locales/LAN se deja
    // deshabilitado por defecto (enableMasterServer=false) para no llenar el log
    // con reintentos de conexión a una IP de terceros que puede estar caída.
    // EN: Master server registration is optional. For local/LAN games it is disabled by default
    //     (enableMasterServer=false) so the log is not flooded with reconnect attempts to a third-party
    //     IP that may be down.
    if (!m_config.enableMasterServer) {
        spdlog::info("GameServer: Master server deshabilitado (modo local), no se registra en el browser publico");
        return;
    }
    if (m_config.masterServer.empty()) {
        spdlog::info("GameServer: No master server configured, skipping registration");
        return;
    }

    // ES: Host ENet separado para la conexión con el master (1 peer, 1 canal).
    // EN: Create a separate ENet host for the master connection (1 peer, 1 channel)
    m_masterHost = enet_host_create(nullptr, 1, 1, 0, 0);
    if (!m_masterHost) {
        spdlog::warn("GameServer: Failed to create master ENet host");
        return;
    }

    ENetAddress masterAddr;
    enet_address_set_host(&masterAddr, m_config.masterServer.c_str());
    masterAddr.port = m_config.masterPort;

    m_masterPeer = enet_host_connect(m_masterHost, &masterAddr, 1, 0);
    if (!m_masterPeer) {
        spdlog::warn("GameServer: Failed to connect to master server at {}:{}",
                     m_config.masterServer, m_config.masterPort);
        enet_host_destroy(m_masterHost);
        m_masterHost = nullptr;
        return;
    }

    spdlog::info("GameServer: Connecting to master server at {}:{}...",
                 m_config.masterServer, m_config.masterPort);
}

// ES: MS_Register: se anuncia en el master (nombre, puerto, jugadores, hora, PvP, IP externa por UPnP).
// EN: MS_Register: announces the server to the master (name, port, players, time, PvP, UPnP external IP).
void GameServer::SendMasterRegister() {
    if (!m_masterPeer || !m_masterConnected) return;

    PacketWriter writer;
    writer.WriteHeader(MessageType::MS_Register);

    MsgMasterRegister msg{};
    msg.protocolVersion = KMP_PROTOCOL_VERSION;
    msg.gamePort = m_config.port;
    msg.currentPlayers = static_cast<uint8_t>(m_players.size());
    msg.maxPlayers = static_cast<uint8_t>(m_config.maxPlayers);
    msg.timeOfDay = m_timeOfDay;
    msg.pvpEnabled = m_config.pvpEnabled ? 1 : 0;
    strncpy(msg.serverName, m_config.serverName.c_str(), sizeof(msg.serverName) - 1);

    // ES: IP externa desde UPnP si la hay; si va vacía, el master usa la IP de origen del paquete.
    // EN: Try to fill external IP from UPnP discovery
    std::string extIP = m_upnp.GetExternalIP();
    if (!extIP.empty()) {
        strncpy(msg.externalIP, extIP.c_str(), sizeof(msg.externalIP) - 1);
    }
    // If empty, master server will use the peer's IP

    writer.WriteRaw(&msg, sizeof(msg));

    ENetPacket* packet = enet_packet_create(writer.Data(), writer.Size(),
                                             ENET_PACKET_FLAG_RELIABLE);
    if (!packet) {
        spdlog::error("Failed to create packet ({} bytes)", writer.Size());
        return;
    }
    enet_peer_send(m_masterPeer, 0, packet);
    enet_host_flush(m_masterHost);

    spdlog::info("GameServer: Registered with master server (name='{}', port={})",
                 m_config.serverName, m_config.port);
}

// ES: MS_Heartbeat: latido periódico (cada 30 s) con puerto, jugadores y hora.
// EN: MS_Heartbeat: periodic beat (every 30 s) with port, players and time.
void GameServer::SendMasterHeartbeat() {
    if (!m_masterPeer || !m_masterConnected) return;

    PacketWriter writer;
    writer.WriteHeader(MessageType::MS_Heartbeat);

    MsgMasterHeartbeat msg{};
    msg.gamePort = m_config.port;
    msg.currentPlayers = static_cast<uint8_t>(m_players.size());
    msg.maxPlayers = static_cast<uint8_t>(m_config.maxPlayers);
    msg.timeOfDay = m_timeOfDay;

    writer.WriteRaw(&msg, sizeof(msg));

    ENetPacket* packet = enet_packet_create(writer.Data(), writer.Size(),
                                             ENET_PACKET_FLAG_RELIABLE);
    if (!packet) {
        spdlog::error("Failed to create packet ({} bytes)", writer.Size());
        return;
    }
    enet_peer_send(m_masterPeer, 0, packet);
}

// ES: MS_Deregister: baja del listado al apagar.
// EN: MS_Deregister: removes the server from the list on shutdown.
void GameServer::SendMasterDeregister() {
    if (!m_masterPeer || !m_masterConnected) return;

    PacketWriter writer;
    writer.WriteHeader(MessageType::MS_Deregister);

    ENetPacket* packet = enet_packet_create(writer.Data(), writer.Size(),
                                             ENET_PACKET_FLAG_RELIABLE);
    if (!packet) {
        spdlog::error("Failed to create packet ({} bytes)", writer.Size());
        return;
    }
    enet_peer_send(m_masterPeer, 0, packet);
    enet_host_flush(m_masterHost);
}

// ES: Llamado cada tick: procesa eventos del host del master (conexión -> registro; desconexión ->
//     preparar reintento), envía el heartbeat y reconecta con espera exponencial (5, 10, 20, 40, 60 s).
// EN: Called every tick: handles master host events (connect -> register; disconnect -> prepare a
//     retry), sends the heartbeat and reconnects with exponential backoff (5, 10, 20, 40, 60 s).
void GameServer::UpdateMasterConnection(float deltaTime) {
    if (!m_masterHost) return;

    // ES: Procesa los eventos de la conexión con el master.
    // EN: Poll master connection events
    ENetEvent event;
    while (enet_host_service(m_masterHost, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                m_masterConnected = true;
                m_masterReconnectDelay = 5.f; // Reset backoff on success
                spdlog::info("GameServer: Connected to master server");
                SendMasterRegister();
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                m_masterConnected = false;
                m_masterPeer = nullptr;
                // ES: Bajado a debug: un master caído NO debe escupir warnings en partida local.
                // EN: Lowered to debug: a down master must not spit warnings during a local game.
                spdlog::debug("GameServer: Disconnected from master server — will retry in {:.0f}s",
                             m_masterReconnectDelay);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                enet_packet_destroy(event.packet);
                break;
            default:
                break;
        }
    }

    // ES: Heartbeat si hay conexión.
    // EN: Send heartbeat if connected
    if (m_masterConnected) {
        m_timeSinceMasterHeartbeat += deltaTime;
        if (m_timeSinceMasterHeartbeat >= m_masterHeartbeatInterval) {
            SendMasterHeartbeat();
            m_timeSinceMasterHeartbeat = 0.f;
        }
    } else if (m_masterPeer == nullptr && !m_config.masterServer.empty()) {
        // ES: Reconexión automática con espera exponencial (5 s -> 10 s -> 20 s -> 40 s -> máx. 60 s).
        // EN: Auto-reconnect with exponential backoff (5s → 10s → 20s → 40s → max 60s)
        m_masterReconnectTimer += deltaTime;
        if (m_masterReconnectTimer >= m_masterReconnectDelay) {
            m_masterReconnectTimer = 0.f;
            spdlog::info("GameServer: Reconnecting to master server at {}:{}...",
                         m_config.masterServer, m_config.masterPort);

            ENetAddress masterAddr;
            enet_address_set_host(&masterAddr, m_config.masterServer.c_str());
            masterAddr.port = m_config.masterPort;
            m_masterPeer = enet_host_connect(m_masterHost, &masterAddr, 1, 0);
            if (!m_masterPeer) {
                spdlog::warn("GameServer: Master reconnect failed to start");
            }

            // ES: Dobla la espera (tope 60 s).
            // EN: Exponential backoff (cap at 60s)
            m_masterReconnectDelay = std::min(m_masterReconnectDelay * 2.f, 60.f);
        }
    }

    enet_host_flush(m_masterHost);
}

// ES: ── "Listo" de jugador tras cargar la partida ──
//     Se llama al recibir C2S_PlayerReady (después de OnGameLoaded en el cliente). Cuando hay más de
//     un jugador y TODOS están listos se envía S2C_AllPlayersReady. Con un solo jugador nunca se envía.
// EN: ── Player Ready Handshake ──
// Called when a client sends C2S_PlayerReady (after OnGameLoaded)
// Server tracks ready players and broadcasts S2C_AllPlayersReady when ALL are ready
void GameServer::HandlePlayerReady(ConnectedPlayer& player) {
    if (player.isReady) {
        spdlog::debug("GameServer: Player {} already marked ready", player.id);
        return;
    }

    player.isReady = true;
    spdlog::info("GameServer: Player {} ({}) is READY ({}/{} players ready)",
                 player.id, player.name, GetReadyPlayerCount(), GetConnectedPlayerCount());

    // ES: Comprueba si ya están listos TODOS los conectados.
    // EN: Check if ALL connected players are now ready
    int connected = GetConnectedPlayerCount();
    int ready = GetReadyPlayerCount();

    if (connected > 1 && ready == connected) {
        // ES: TODOS LISTOS: aviso a todos.
        // EN: ALL PLAYERS READY - Broadcast to everyone
        spdlog::info("GameServer: ALL {} PLAYERS READY - Broadcasting AllPlayersReady", connected);

        PacketWriter writer;
        writer.WriteHeader(MessageType::S2C_AllPlayersReady);
        Broadcast(writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);

        // ES: Log por jugador.
        // EN: Log for each player
        for (auto& [pid, p] : m_players) {
            spdlog::info("GameServer: Sent AllPlayersReady to {} ({})", p.id, p.name);
        }
    } else {
        spdlog::info("GameServer: Waiting for more players ({}/{} ready)", ready, connected);
    }
}

// ES: Número de jugadores con isReady = true.
// EN: Number of players with isReady = true.
int GameServer::GetReadyPlayerCount() const {
    int count = 0;
    for (const auto& [pid, p] : m_players) {
        if (p.isReady) count++;
    }
    return count;
}

// ES: Número de jugadores con handshake hecho (equivale a m_players.size()).
// EN: Number of handshaken players (equivalent to m_players.size()).
int GameServer::GetConnectedPlayerCount() const {
    int count = 0;
    for (const auto& [pid, p] : m_players) {
        (void)pid; // Suppress unused warning
        count++;
    }
    return count;
}

} // namespace kmp
