// ES: Cliente de red del mod, sobre ENet (UDP con canales fiables y no fiables).
//     Gestiona la conexión con el servidor dedicado (bloqueante o asíncrona), el
//     bombeo de eventos de ENet y el envío de paquetes por los canales del protocolo.
//     Todas las operaciones de ENet se protegen con un mutex porque ENet no es thread-safe.
// EN: The mod's network client, built on ENet (UDP with reliable and unreliable
//     channels). It handles the connection to the dedicated server (blocking or
//     async), pumping ENet events and sending packets on the protocol channels.
//     All ENet operations are guarded by a mutex because ENet is not thread-safe.
#pragma once
#include "kmp/types.h"
#include "kmp/constants.h"
#include <enet/enet.h>
#include <cstdint>
#include <string>
#include <functional>
#include <mutex>
#include <vector>
#include <atomic>

namespace kmp {

// ES: Cliente ENet con un único par (el servidor).
// EN: ENet client with a single peer (the server).
class NetworkClient {
public:
    // ES: Callback que recibe cada paquete entrante (datos, tamaño, canal).
    // EN: Callback that receives every incoming packet (data, size, channel).
    using PacketCallback = std::function<void(const uint8_t* data, size_t size, int channel)>;

    // ES: Inicializa ENet y crea el host cliente / libera todo.
    // EN: Initializes ENet and creates the client host / releases everything.
    bool Initialize();
    void Shutdown();

    // ES: Conexión bloqueante (antigua), conexión asíncrona (preferida), desconexión y
    //     bombeo de eventos (llamar a menudo; invoca el callback de paquetes).
    // EN: Blocking connect (legacy), async connect (preferred), disconnect and event
    //     pump (call often; it invokes the packet callback).
    bool Connect(const std::string& address, uint16_t port);       // Blocking (legacy)
    bool ConnectAsync(const std::string& address, uint16_t port); // Non-blocking
    void Disconnect();
    void Update(); // Pump ENet events - call frequently

    // ES: Envío por canales concretos: fiable ordenado, fiable no ordenado y no fiable
    //     secuenciado.
    // Send on specific channels
    void SendReliable(const uint8_t* data, size_t len);
    void SendReliableUnordered(const uint8_t* data, size_t len);
    void SendUnreliable(const uint8_t* data, size_t len);

    // ES: Fija el callback de paquetes (bajo el mutex de ENet).
    // EN: Sets the packet callback (under the ENet mutex).
    void SetPacketCallback(PacketCallback cb) {
        std::lock_guard lock(m_enetMutex);
        m_callback = std::move(cb);
    }

    // ES: Estado de la conexión, ping (RTT de ENet en ms) y dirección/puerto del servidor.
    // EN: Connection state, ping (ENet RTT in ms) and server address/port.
    bool IsConnected() const { return m_connected; }
    bool IsConnecting() const { return m_connecting; }
    uint32_t GetPing() const;
    const std::string& GetServerAddress() const { return m_serverAddr; }
    uint16_t GetServerPort() const { return m_serverPort; }

private:
    // ES: Host ENet local, par del servidor, callback y mutex que protege TODA operación de
    //     ENet (host/par). Nota: Connect() y Disconnect() llaman a ENet sin tomar este mutex.
    // EN: Local ENet host, server peer, callback and the mutex guarding ALL ENet
    //     operations (host/peer). Note: Connect() and Disconnect() call ENet without it.
    ENetHost*   m_host       = nullptr;
    ENetPeer*   m_serverPeer = nullptr;
    PacketCallback m_callback;
    std::mutex  m_enetMutex; // Protects ALL ENet host/peer operations (not thread-safe)

    // ES: Estado de conexión (atómico, se lee desde varios hilos) y datos del servidor.
    //     m_connectStartTime no se usa en client.cpp.
    // EN: Connection state (atomic, read from several threads) and server data.
    //     m_connectStartTime is not used in client.cpp.
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_connecting{false};
    float       m_connectStartTime = 0.f;
    std::string m_serverAddr;
    uint16_t    m_serverPort = 0;
    bool        m_initialized = false;

    // ES: Envío genérico: crea el paquete ENet con los flags y lo manda por el canal.
    // EN: Generic send: creates the ENet packet with the flags and sends it on the channel.
    void Send(const uint8_t* data, size_t len, int channel, uint32_t flags);
};

} // namespace kmp
