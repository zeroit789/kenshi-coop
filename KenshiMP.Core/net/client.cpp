// ES: Implementación de NetworkClient (ver client.h).
// EN: NetworkClient implementation (see client.h).
#include "client.h"
#include <spdlog/spdlog.h>

namespace kmp {

// ES: Inicializa la librería ENet y crea un host cliente con 1 conexión saliente,
//     KMP_CHANNEL_COUNT canales y los límites de ancho de banda del protocolo.
// EN: Initializes the ENet library and creates a client host with 1 outgoing
//     connection, KMP_CHANNEL_COUNT channels and the protocol bandwidth limits.
bool NetworkClient::Initialize() {
    if (m_initialized) return true;

    if (enet_initialize() != 0) {
        spdlog::error("NetworkClient: Failed to initialize ENet");
        return false;
    }

    m_host = enet_host_create(nullptr, 1, KMP_CHANNEL_COUNT,
                              KMP_DOWNSTREAM_LIMIT, KMP_UPSTREAM_LIMIT);
    if (!m_host) {
        spdlog::error("NetworkClient: Failed to create ENet host");
        enet_deinitialize();
        return false;
    }

    m_initialized = true;
    spdlog::info("NetworkClient: Initialized");
    return true;
}

// ES: Desconecta, destruye el host y desinicializa ENet.
// EN: Disconnects, destroys the host and deinitializes ENet.
void NetworkClient::Shutdown() {
    Disconnect();
    if (m_host) {
        enet_host_destroy(m_host);
        m_host = nullptr;
    }
    if (m_initialized) {
        enet_deinitialize();
        m_initialized = false;
    }
}

// ES: Conexión bloqueante: espera hasta KMP_CONNECT_TIMEOUT_MS al evento CONNECT.
//     Devuelve true si conecta.
// EN: Blocking connect: waits up to KMP_CONNECT_TIMEOUT_MS for the CONNECT event.
//     Returns true on success.
bool NetworkClient::Connect(const std::string& address, uint16_t port) {
    // ES: Conexión bloqueante: se mantiene por compatibilidad, pero mejor ConnectAsync().
    // Blocking connect — kept for compatibility but prefer ConnectAsync()
    if (!m_initialized || m_connected) return false;

    ENetAddress addr;
    enet_address_set_host(&addr, address.c_str());
    addr.port = port;

    m_serverPeer = enet_host_connect(m_host, &addr, KMP_CHANNEL_COUNT, 0);
    if (!m_serverPeer) {
        spdlog::error("NetworkClient: Failed to initiate connection to {}:{}", address, port);
        return false;
    }

    // ES: Esperar la conexión (con límite de tiempo).
    // Wait for connection (with timeout)
    ENetEvent event;
    if (enet_host_service(m_host, &event, KMP_CONNECT_TIMEOUT_MS) > 0 &&
        event.type == ENET_EVENT_TYPE_CONNECT) {
        m_connected = true;
        m_serverAddr = address;
        m_serverPort = port;
        // ES: Poner un timeout de sesión amplio (el de conexión era 5 s, poco para jugar).
        // Set generous session timeout (connect timeout was 5s, too short for gameplay)
        enet_peer_timeout(m_serverPeer, 0, 30000, 60000);
        spdlog::info("NetworkClient: Connected to {}:{}", address, port);
        return true;
    }

    enet_peer_reset(m_serverPeer);
    m_serverPeer = nullptr;
    spdlog::error("NetworkClient: Connection to {}:{} timed out", address, port);
    return false;
}

// ES: Conexión asíncrona: lanza el intento y vuelve al momento; Update() recibirá el
//     evento CONNECT y marcará la conexión como establecida.
// EN: Async connect: starts the attempt and returns immediately; Update() will get the
//     CONNECT event and mark the connection as established.
bool NetworkClient::ConnectAsync(const std::string& address, uint16_t port) {
    if (!m_initialized || m_connected || m_connecting) return false;

    ENetAddress addr;
    enet_address_set_host(&addr, address.c_str());
    addr.port = port;

    m_serverPeer = enet_host_connect(m_host, &addr, KMP_CHANNEL_COUNT, 0);
    if (!m_serverPeer) {
        spdlog::error("NetworkClient: Failed to initiate async connection to {}:{}", address, port);
        return false;
    }

    // ES: Timeout de conexión razonable (KMP_CONNECT_TIMEOUT_MS, 5 s).
    // Set a reasonable connect timeout (5 seconds)
    enet_peer_timeout(m_serverPeer, 0, KMP_CONNECT_TIMEOUT_MS, KMP_CONNECT_TIMEOUT_MS);

    m_connecting = true;
    m_serverAddr = address;
    m_serverPort = port;
    spdlog::info("NetworkClient: Async connecting to {}:{}...", address, port);
    return true;
}

// ES: Desconexión ordenada: pide la desconexión y espera hasta 1 s el acuse; si no
//     llega (o no estábamos conectados), resetea el par a la fuerza.
// EN: Graceful disconnect: requests disconnection and waits up to 1 s for the ack; if
//     it does not arrive (or we were not connected), force-resets the peer.
void NetworkClient::Disconnect() {
    if (m_serverPeer) {
        if (m_connected) {
            enet_peer_disconnect(m_serverPeer, 0);

            // ES: Esperar un poco el acuse de desconexión (se descartan los paquetes que lleguen).
            // Wait briefly for disconnect acknowledgment
            ENetEvent event;
            bool disconnected = false;
            while (enet_host_service(m_host, &event, 1000) > 0) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    disconnected = true;
                    break;
                }
            }

            if (!disconnected) {
                enet_peer_reset(m_serverPeer);
            }
        } else {
            enet_peer_reset(m_serverPeer);
        }
    }

    m_serverPeer = nullptr;
    m_connected = false;
    m_connecting = false;
    spdlog::info("NetworkClient: Disconnected (state reset)");
}

// ES: Bombea los eventos de ENet (sin bloquear): conexión establecida, paquete recibido
//     (se pasa al callback) y desconexión. Ojo: el callback se ejecuta con m_enetMutex
//     tomado; enviar desde dentro del callback provocaría un interbloqueo.
// EN: Pumps ENet events (non-blocking): connection established, packet received
//     (passed to the callback) and disconnection. Note: the callback runs with
//     m_enetMutex held; sending from inside the callback would deadlock.
void NetworkClient::Update() {
    if (!m_host) return;

    ENetEvent event;
    // ES: El lock cubre enet_host_service, que comparte estado interno con enet_peer_send.
    //     Con timeout 0 no bloquea, así que la contención es mínima.
    // Lock covers enet_host_service which shares internal state with enet_peer_send.
    // The 0 timeout makes this non-blocking, so lock contention is minimal.
    std::lock_guard lock(m_enetMutex);
    while (enet_host_service(m_host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            // ES: La conexión asíncrona ha tenido éxito.
            // Async connection succeeded
            m_connected = true;
            m_connecting = false;
            // ES: Timeout de sesión amplio (30 s mín., 60 s máx.); el de conexión (5 s) era
            //     demasiado corto para una partida.
            // Set a generous session timeout (30s min, 60s max).
            // The connect timeout was 5s — way too short for ongoing gameplay.
            enet_peer_timeout(m_serverPeer, 0, 30000, 60000);
            spdlog::info("NetworkClient: Connected to {}:{} (timeout set to 30-60s)",
                         m_serverAddr, m_serverPort);
            break;

        // ES: Paquete recibido: pasarlo al callback y liberarlo.
        // EN: Packet received: hand it to the callback and free it.
        case ENET_EVENT_TYPE_RECEIVE:
            if (m_callback) {
                m_callback(event.packet->data, event.packet->dataLength,
                          event.channelID);
            }
            enet_packet_destroy(event.packet);
            break;

        // ES: El servidor nos ha desconectado (o se ha perdido la conexión).
        // EN: The server disconnected us (or the connection was lost).
        case ENET_EVENT_TYPE_DISCONNECT:
            spdlog::warn("NetworkClient: Disconnected from server (reason: {})",
                         event.data);
            m_connected = false;
            m_connecting = false;
            m_serverPeer = nullptr;
            break;

        default:
            break;
        }
    }
}

// ES: Crea un paquete ENet con los flags dados y lo encola en el canal (solo si conectado).
// EN: Creates an ENet packet with the given flags and queues it on the channel (only if connected).
void NetworkClient::Send(const uint8_t* data, size_t len, int channel, uint32_t flags) {
    if (!m_connected || !m_serverPeer) return;

    std::lock_guard lock(m_enetMutex);
    ENetPacket* packet = enet_packet_create(data, len, flags);
    if (packet) {
        enet_peer_send(m_serverPeer, channel, packet);
    }
}

// ES: Canal fiable y ordenado.
// EN: Reliable ordered channel.
void NetworkClient::SendReliable(const uint8_t* data, size_t len) {
    Send(data, len, KMP_CHANNEL_RELIABLE_ORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: Canal fiable no ordenado.
// EN: Reliable unordered channel.
void NetworkClient::SendReliableUnordered(const uint8_t* data, size_t len) {
    Send(data, len, KMP_CHANNEL_RELIABLE_UNORDERED, ENET_PACKET_FLAG_RELIABLE);
}

// ES: Canal no fiable secuenciado (posiciones).
// EN: Unreliable sequenced channel (positions).
void NetworkClient::SendUnreliable(const uint8_t* data, size_t len) {
    // ES: flags=0 significa no fiable + secuenciado: ENet descarta automáticamente los
    //     paquetes tardíos/desordenados, así los datos de posición viejos no pisan a los
    //     actuales. ENET_PACKET_FLAG_UNSEQUENCED entregaría TODOS sin importar el orden.
    // flags=0 means unreliable + sequenced. ENet drops late/out-of-order packets
    // automatically, preventing stale position data from overwriting current state.
    // ENET_PACKET_FLAG_UNSEQUENCED would deliver ALL packets regardless of order.
    Send(data, len, KMP_CHANNEL_UNRELIABLE_SEQ, 0);
}

// ES: Tiempo de ida y vuelta (RTT) del par servidor en ms, o 0 si no hay conexión.
// EN: Round-trip time (RTT) of the server peer in ms, or 0 if not connected.
uint32_t NetworkClient::GetPing() const {
    if (!m_serverPeer) return 0;
    return m_serverPeer->roundTripTime;
}

} // namespace kmp
