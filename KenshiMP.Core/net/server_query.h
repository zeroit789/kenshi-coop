// ES: Cliente de consulta de servidores para el navegador de servidores del menú.
//     Pregunta directamente a servidores (C2S_ServerQuery → S2C_ServerInfo) o al
//     servidor maestro (MS_QueryList → MS_ServerList) usando un host ENet aparte,
//     sin interferir con la conexión de juego.
// EN: Server query client for the in-menu server browser.
//     It asks servers directly (C2S_ServerQuery → S2C_ServerInfo) or the master
//     server (MS_QueryList → MS_ServerList) using a separate ENet host, without
//     interfering with the game connection.
#pragma once
#include "kmp/types.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/constants.h"
#include <enet/enet.h>
#include <string>
#include <vector>
#include <mutex>
#include <functional>

namespace kmp {

// ES: Resultado de consultar un servidor: dirección, nombre, jugadores, ping y estado.
// EN: Result of querying one server: address, name, players, ping and status.
struct ServerQueryResult {
    std::string address;
    uint16_t    port = 0;
    std::string serverName;
    uint8_t     currentPlayers = 0;
    uint8_t     maxPlayers = 0;
    uint32_t    ping = 0;
    bool        online = false;
    bool        pending = true; // Still waiting for response
};

// ES: Consulta asíncrona ligera de servidores.
//     Usa un ENetHost aparte para no interferir con la conexión de juego.
//     Llamar a QueryServer() para empezar y a Update() cada frame para procesar respuestas.
// Lightweight async server query.
// Uses a separate ENetHost to query servers without interfering with the game connection.
// Call QueryServer() to start a query, then PumpResults() each frame to process responses.
class ServerQueryClient {
public:
    // ES: Crea / destruye el host ENet de consultas.
    // EN: Creates / destroys the query ENet host.
    bool Initialize();
    void Shutdown();

    // ES: Empieza a consultar un servidor directamente. No bloquea.
    // Start querying a server directly. Non-blocking.
    void QueryServer(const std::string& address, uint16_t port);

    // ES: Pide al servidor maestro la lista de servidores registrados. No bloquea.
    // Query the master server for all registered game servers. Non-blocking.
    void QueryMasterServer(const std::string& masterAddress, uint16_t masterPort);

    // ES: Bombea eventos de ENet y procesa respuestas. Llamar cada frame.
    // Pump ENet events and process responses. Call each frame.
    void Update();

    // ES: Copia de los resultados actuales (thread-safe).
    // Get current results (thread-safe copy).
    std::vector<ServerQueryResult> GetResults();

    // ES: Borra todos los resultados y consultas pendientes.
    // Clear all results and pending queries.
    void Clear();

    // ES: ¿Hay consultas en curso?
    // EN: Are there queries in progress?
    bool IsQueryActive() const { return m_active; }

private:
    // ES: Consulta en curso: par ENet, dirección, hora de inicio y si es al maestro.
    // EN: Query in progress: ENet peer, address, start time and whether it targets the master.
    struct PendingQuery {
        ENetPeer*   peer = nullptr;
        std::string address;
        uint16_t    port = 0;
        float       startTime = 0.f;
        bool        isMasterQuery = false; // True if this is a master server query
    };

    // ES: Host de consultas, estado, consultas pendientes, resultados (protegidos por
    //     m_mutex) y m_elapsed (no se usa en server_query.cpp).
    // EN: Query host, state, pending queries, results (guarded by m_mutex) and m_elapsed
    //     (not used in server_query.cpp).
    ENetHost* m_host = nullptr;
    bool m_initialized = false;
    bool m_active = false;

    std::vector<PendingQuery> m_pending;
    std::vector<ServerQueryResult> m_results;
    std::mutex m_mutex;
    float m_elapsed = 0.f;
};

} // namespace kmp
