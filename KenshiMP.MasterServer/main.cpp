// ES: Master Server de KenshiMP
//     Registro centralizado para el navegador de servidores.
//     Los servidores de juego se registran y mandan latidos; los clientes piden la lista viva.
//     Usa ENet en el puerto 27801 (configurable en master.json).
// EN: KenshiMP Master Server
//     Centralized registry for the server browser.
//     Game servers register via heartbeat; clients query for the live list.
//     Uses ENet on port 27801 (configurable).

#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/constants.h"
#include <enet/enet.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <fstream>

using json = nlohmann::json;

namespace {

// ES: Un servidor de juego registrado: datos que se muestran en el navegador, hora del
//     último latido (para caducarlo) y la conexión ENet con la que se registró.
// EN: A registered game server: data shown in the browser, time of the last heartbeat
//     (to expire it) and the ENet connection it registered from.
struct RegisteredServer {
    std::string serverName;
    std::string address;     // External IP (from peer address or self-reported)
    uint16_t    port = 0;
    uint8_t     currentPlayers = 0;
    uint8_t     maxPlayers = 16;
    float       timeOfDay = 0.5f;
    uint8_t     pvpEnabled = 1;
    std::chrono::steady_clock::time_point lastHeartbeat;
    ENetPeer*   peer = nullptr; // Server's connection to master
};

// ES: Tabla de servidores registrados. Clave: "ip:puerto".
//     Ojo: el hilo de consola ("status") la lee sin mutex mientras el bucle principal
//     la modifica (carrera de datos; sin protección).
// EN: Registered server table. Key: "ip:port"
//     Note: the console thread ("status") reads it without a mutex while the main loop
//     modifies it (data race; unprotected).
std::unordered_map<std::string, RegisteredServer> g_servers;
// ES: Bandera de ejecución; la ponen a false Ctrl+C/SIGTERM o el comando "stop".
// EN: Run flag; set to false by Ctrl+C/SIGTERM or the "stop" command.
std::atomic<bool> g_running{true};

// ES: Parámetros: caducidad sin latido, puerto por defecto y máximo de conexiones ENet.
// EN: Parameters: heartbeat expiry, default port and max ENet connections.
constexpr float HEARTBEAT_TIMEOUT_SEC = 90.f; // Remove after 90s no heartbeat
constexpr uint16_t DEFAULT_MASTER_PORT = 27801;
constexpr int MAX_CONNECTIONS = 128;

// ES: Manejador de SIGINT/SIGTERM: pide salir del bucle principal.
// EN: SIGINT/SIGTERM handler: requests the main loop to exit.
void SignalHandler(int) {
    g_running = false;
}

// ES: Devuelve la IP del peer ENet como texto.
// EN: Returns the ENet peer's IP as text.
std::string PeerAddressString(ENetPeer* peer) {
    char buf[64];
    enet_address_get_host_ip(&peer->address, buf, sizeof(buf));
    return std::string(buf);
}

// ES: Construye la clave "ip:puerto" de la tabla de servidores.
// EN: Builds the "ip:port" key of the server table.
std::string MakeKey(const std::string& ip, uint16_t port) {
    return ip + ":" + std::to_string(port);
}

// ES: MS_Register: da de alta o actualiza un servidor de juego. Rechaza versiones de
//     protocolo distintas. Si el servidor no informa su IP externa se usa la IP del peer.
//     Ojo: serverName/externalIP se copian del struct sin forzar el terminador nulo.
// EN: MS_Register: registers or updates a game server. Rejects mismatched protocol
//     versions. If the server doesn't report its external IP, the peer IP is used.
//     Note: serverName/externalIP are copied from the struct without forcing a null terminator.
void HandleRegister(ENetPeer* peer, const uint8_t* data, size_t size) {
    using namespace kmp;
    PacketReader reader(data, size);
    PacketHeader header;
    if (!reader.ReadHeader(header)) return;

    MsgMasterRegister msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    if (msg.protocolVersion != KMP_PROTOCOL_VERSION) {
        spdlog::warn("Master: Rejected server (protocol mismatch: {} vs {})",
                     msg.protocolVersion, KMP_PROTOCOL_VERSION);
        return;
    }

    std::string peerIP = PeerAddressString(peer);

    // ES: Usar la IP real del peer si el servidor no dio ninguna
    // EN: Use peer's actual IP if server didn't provide one
    std::string externalIP = msg.externalIP[0] ? std::string(msg.externalIP) : peerIP;
    std::string key = MakeKey(externalIP, msg.gamePort);

    RegisteredServer srv;
    srv.serverName = msg.serverName;
    srv.address = externalIP;
    srv.port = msg.gamePort;
    srv.currentPlayers = msg.currentPlayers;
    srv.maxPlayers = msg.maxPlayers;
    srv.timeOfDay = msg.timeOfDay;
    srv.pvpEnabled = msg.pvpEnabled;
    srv.lastHeartbeat = std::chrono::steady_clock::now();
    srv.peer = peer;

    bool isNew = (g_servers.find(key) == g_servers.end());
    g_servers[key] = srv;

    if (isNew) {
        spdlog::info("Master: Server registered: '{}' at {} ({}/{} players)",
                     srv.serverName, key, srv.currentPlayers, srv.maxPlayers);
    } else {
        spdlog::debug("Master: Server updated: '{}' at {}", srv.serverName, key);
    }
}

// ES: MS_Heartbeat: refresca jugadores, hora del día y marca de tiempo del servidor.
//     Busca primero por "ipDelPeer:puerto"; si no está (p.ej. se registró con otra IP
//     externa por NAT), busca por el mismo peer ENet y puerto.
// EN: MS_Heartbeat: refreshes players, time of day and the server's timestamp.
//     Looks up "peerIP:port" first; if missing (e.g. registered with another external IP
//     due to NAT), falls back to matching the same ENet peer and port.
void HandleHeartbeat(ENetPeer* peer, const uint8_t* data, size_t size) {
    using namespace kmp;
    PacketReader reader(data, size);
    PacketHeader header;
    if (!reader.ReadHeader(header)) return;

    MsgMasterHeartbeat msg;
    if (!reader.ReadRaw(&msg, sizeof(msg))) return;

    std::string peerIP = PeerAddressString(peer);
    std::string key = MakeKey(peerIP, msg.gamePort);

    auto it = g_servers.find(key);
    if (it != g_servers.end()) {
        it->second.currentPlayers = msg.currentPlayers;
        it->second.maxPlayers = msg.maxPlayers;
        it->second.timeOfDay = msg.timeOfDay;
        it->second.lastHeartbeat = std::chrono::steady_clock::now();
    } else {
        // ES: Latido de un servidor desconocido — comprobar si está registrado con otra clave
        //     (la IP externa puede diferir de la del peer por NAT)
        // EN: Unknown server heartbeat — check if registered under different key
        //     (external IP may differ from peer IP due to NAT)
        for (auto& [k, s] : g_servers) {
            if (s.peer == peer && s.port == msg.gamePort) {
                s.currentPlayers = msg.currentPlayers;
                s.maxPlayers = msg.maxPlayers;
                s.timeOfDay = msg.timeOfDay;
                s.lastHeartbeat = std::chrono::steady_clock::now();
                return;
            }
        }
        spdlog::debug("Master: Heartbeat from unknown server {}", key);
    }
}

// ES: MS_Deregister: el servidor de juego se apaga; se borran todas sus entradas.
//     data/size no se usan (peerIP tampoco).
// EN: MS_Deregister: the game server is shutting down; all its entries are removed.
//     data/size are unused (so is peerIP).
void HandleDeregister(ENetPeer* peer, const uint8_t* data, size_t size) {
    std::string peerIP = PeerAddressString(peer);

    // ES: Borrar todos los servidores de este peer
    // EN: Remove all servers from this peer
    for (auto it = g_servers.begin(); it != g_servers.end(); ) {
        if (it->second.peer == peer) {
            spdlog::info("Master: Server deregistered: '{}' at {}",
                         it->second.serverName, it->first);
            it = g_servers.erase(it);
        } else {
            ++it;
        }
    }
}

// ES: MS_QueryList: responde con MS_ServerList (cabecera + uint16 con el número de
//     servidores + N MsgMasterServerEntry) y desconecta al cliente del navegador.
// EN: MS_QueryList: replies with MS_ServerList (header + uint16 server count +
//     N MsgMasterServerEntry) and disconnects the browser client.
void HandleQueryList(ENetPeer* peer) {
    using namespace kmp;

    // ES: Construir la respuesta con la lista de servidores
    // EN: Build server list response
    PacketWriter writer;
    writer.WriteHeader(MessageType::MS_ServerList);

    // ES: Escribir el número de servidores
    // EN: Write count
    uint16_t count = static_cast<uint16_t>(g_servers.size());
    writer.WriteU16(count);

    // ES: Escribir cada entrada (cadenas truncadas y siempre terminadas en nulo)
    // EN: Write each server entry
    for (auto& [key, srv] : g_servers) {
        MsgMasterServerEntry entry{};
        strncpy(entry.serverName, srv.serverName.c_str(), sizeof(entry.serverName) - 1);
        strncpy(entry.address, srv.address.c_str(), sizeof(entry.address) - 1);
        entry.port = srv.port;
        entry.currentPlayers = srv.currentPlayers;
        entry.maxPlayers = srv.maxPlayers;
        entry.pvpEnabled = srv.pvpEnabled;
        writer.WriteRaw(&entry, sizeof(entry));
    }

    ENetPacket* packet = enet_packet_create(writer.Data(), writer.Size(),
                                             ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, packet);

    spdlog::debug("Master: Sent server list ({} servers) to {}",
                  count, PeerAddressString(peer));

    // ES: Desconectar al cliente del navegador tras enviar la lista (consulta ligera)
    // EN: Disconnect browser client after sending list (lightweight query)
    enet_peer_disconnect_later(peer, 0);
}

// ES: Borra los servidores que llevan más de HEARTBEAT_TIMEOUT_SEC sin latido.
// EN: Removes servers with no heartbeat for more than HEARTBEAT_TIMEOUT_SEC.
void PruneStaleServers() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = g_servers.begin(); it != g_servers.end(); ) {
        float elapsed = std::chrono::duration<float>(now - it->second.lastHeartbeat).count();
        if (elapsed > HEARTBEAT_TIMEOUT_SEC) {
            spdlog::info("Master: Pruned stale server '{}' at {} ({}s since heartbeat)",
                         it->second.serverName, it->first, static_cast<int>(elapsed));
            it = g_servers.erase(it);
        } else {
            ++it;
        }
    }
}

// ES: Despacha un paquete recibido según su tipo de mensaje. Cada manejador vuelve a
//     leer la cabecera desde el principio del búfer.
// EN: Dispatches a received packet by message type. Each handler re-reads the header
//     from the start of the buffer.
void HandlePacket(ENetPeer* peer, const uint8_t* data, size_t size) {
    using namespace kmp;

    if (size < sizeof(PacketHeader)) return;
    PacketReader reader(data, size);
    PacketHeader header;
    if (!reader.ReadHeader(header)) return;

    // ES: Lector desde el principio para que los manejadores relean la cabecera
    //     (no se usa: los manejadores crean su propio lector)
    // EN: Reset reader to beginning so handlers can re-read header
    //     (unused: handlers build their own reader)
    PacketReader fullReader(data, size);

    switch (header.type) {
        case MessageType::MS_Register:
            HandleRegister(peer, data, size);
            break;
        case MessageType::MS_Heartbeat:
            HandleHeartbeat(peer, data, size);
            break;
        case MessageType::MS_Deregister:
            HandleDeregister(peer, data, size);
            break;
        case MessageType::MS_QueryList:
            HandleQueryList(peer);
            break;
        default:
            spdlog::debug("Master: Unknown message type 0x{:02X} from {}",
                          static_cast<uint8_t>(header.type), PeerAddressString(peer));
            break;
    }
}

// ES: Configuración del master (master.json): puerto y fichero de log.
//     Ojo: logFile se carga/guarda pero main() usa siempre "KenshiMP_Master.log".
// EN: Master configuration (master.json): port and log file.
//     Note: logFile is loaded/saved but main() always uses "KenshiMP_Master.log".
struct MasterConfig {
    uint16_t    port = DEFAULT_MASTER_PORT;
    std::string logFile = "KenshiMP_Master.log";

    // ES: Lee el JSON; false si no existe o es inválido.
    // EN: Reads the JSON; false if missing or invalid.
    bool Load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return false;
        try {
            json j;
            file >> j;
            if (j.contains("port"))    port    = j["port"].get<uint16_t>();
            if (j.contains("logFile")) logFile = j["logFile"].get<std::string>();
            return true;
        } catch (...) {
            return false;
        }
    }

    // ES: Escribe el JSON indentado; false si no se puede abrir el fichero.
    // EN: Writes indented JSON; false if the file can't be opened.
    bool Save(const std::string& path) const {
        json j;
        j["port"] = port;
        j["logFile"] = logFile;
        std::ofstream file(path);
        if (!file.is_open()) return false;
        file << j.dump(2);
        return true;
    }
};

} // anonymous namespace

// ES: Punto de entrada: configura logs, carga master.json (o la ruta del argv[1]),
//     crea el host ENet, arranca un hilo de consola y ejecuta el bucle de red.
// EN: Entry point: sets up logging, loads master.json (or argv[1]), creates the ENet
//     host, starts a console thread and runs the network loop.
int main(int argc, char* argv[]) {
    // ES: Logs a consola y a fichero (el fichero se trunca en cada arranque)
    // EN: Setup logging
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("KenshiMP_Master.log", true);
    auto logger = std::make_shared<spdlog::logger>("master",
        spdlog::sinks_init_list{consoleSink, fileSink});
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);

    // ES: Cargar la config; si no existe se crea con los valores por defecto
    // EN: Load config
    MasterConfig config;
    std::string configPath = (argc > 1) ? argv[1] : "master.json";
    if (!config.Load(configPath)) {
        spdlog::info("Master: No config found at '{}', using defaults", configPath);
        config.Save(configPath);
    }

    // ES: Manejadores de señales para un apagado limpio
    // EN: Signal handler
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // ES: Inicializar ENet
    // EN: Initialize ENet
    if (enet_initialize() != 0) {
        spdlog::error("Master: Failed to initialize ENet");
        return 1;
    }

    // ES: Crear el host ENet escuchando en todas las interfaces (1 canal, sin límite de ancho de banda)
    // EN: Create host
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = config.port;

    ENetHost* host = enet_host_create(&address, MAX_CONNECTIONS, 1, 0, 0);
    if (!host) {
        spdlog::error("Master: Failed to create ENet host on port {}", config.port);
        enet_deinitialize();
        return 1;
    }

    spdlog::info("=== KenshiMP Master Server ===");
    spdlog::info("Listening on port {}", config.port);
    spdlog::info("Press Ctrl+C to stop");

    float timeSincePrune = 0.f;
    auto lastTick = std::chrono::steady_clock::now();

    // ES: Hilo de consola para comandos de admin (status, stop/quit/exit, help).
    //     Se desacopla (detach) y queda bloqueado en getline hasta que el proceso termina.
    // EN: Console thread for admin commands
    //     It is detached and stays blocked in getline until the process exits.
    std::thread consoleThread([&]() {
        std::string line;
        while (g_running && std::getline(std::cin, line)) {
            if (line == "stop" || line == "quit" || line == "exit") {
                g_running = false;
            } else if (line == "status") {
                spdlog::info("=== Master Status ===");
                spdlog::info("Registered servers: {}", g_servers.size());
                for (auto& [key, srv] : g_servers) {
                    float age = std::chrono::duration<float>(
                        std::chrono::steady_clock::now() - srv.lastHeartbeat).count();
                    spdlog::info("  '{}' at {} ({}/{}) last heartbeat {:.0f}s ago",
                                 srv.serverName, key, srv.currentPlayers, srv.maxPlayers, age);
                }
            } else if (line == "help") {
                spdlog::info("Commands: status, stop/quit/exit, help");
            }
        }
    });
    consoleThread.detach();

    // ES: Bucle principal: procesa eventos ENet, purga servidores caducados y duerme 10 ms.
    // EN: Main loop
    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastTick).count();
        lastTick = now;

        // ES: Procesar todos los eventos ENet pendientes (sin espera)
        // EN: Poll ENet events
        ENetEvent event;
        while (enet_host_service(host, &event, 0) > 0) {
            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    spdlog::debug("Master: Peer connected from {}", PeerAddressString(event.peer));
                    break;

                case ENET_EVENT_TYPE_RECEIVE:
                    HandlePacket(event.peer, event.packet->data, event.packet->dataLength);
                    enet_packet_destroy(event.packet);
                    break;

                case ENET_EVENT_TYPE_DISCONNECT: {
                    std::string peerIP = PeerAddressString(event.peer);
                    // ES: Borrar los servidores registrados por este peer
                    // EN: Remove any servers from this peer
                    for (auto it = g_servers.begin(); it != g_servers.end(); ) {
                        if (it->second.peer == event.peer) {
                            spdlog::info("Master: Server disconnected: '{}' at {}",
                                         it->second.serverName, it->first);
                            it = g_servers.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    break;
                }

                default:
                    break;
            }
        }

        // ES: Purgar servidores caducados cada 30 segundos
        // EN: Prune stale servers every 30 seconds
        timeSincePrune += dt;
        if (timeSincePrune >= 30.f) {
            timeSincePrune = 0.f;
            PruneStaleServers();
        }

        enet_host_flush(host);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // ES: Apagado: liberar el host y ENet
    // EN: Shutdown: release the host and ENet
    spdlog::info("Master: Shutting down...");
    enet_host_destroy(host);
    enet_deinitialize();
    return 0;
}
