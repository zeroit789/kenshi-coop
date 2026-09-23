// ES: Overlay del cliente. Pese al nombre ya no dibuja nada (la antigua UI ImGui/GDI se
//     sustituyó por NativeMenu y NativeHud de MyGUI): ahora es el controlador por frame
//     del estado de conexión (detección de partida cargada, auto-conexión, reintentos,
//     handshake, detección de desconexión) y del navegador de servidores. También guarda
//     un historial de chat y una lista de jugadores que no se muestran en ningún sitio.
// EN: Client overlay. Despite the name it no longer draws anything (the old ImGui/GDI UI
//     was replaced by MyGUI's NativeMenu and NativeHud): it is now the per-frame
//     controller of connection state (game-load detection, auto-connect, retries,
//     handshake, disconnect detection) and of the server browser. It also keeps a chat
//     history and a player list that are not displayed anywhere.
#pragma once
#include "native_menu.h"
#include "../net/server_query.h"
#include "kmp/types.h"
#include <string>
#include <vector>
#include <mutex>
#include <deque>
#include <chrono>

namespace kmp {

// ES: Controlador de conexión + dueño del NativeMenu y del cliente de consulta de servidores.
// EN: Connection controller + owner of the NativeMenu and the server query client.
class Overlay {
public:
    // ES: Update se llama cada frame desde el hook de Present (auto-conexión, estado de
    //     conexión); Shutdown cierra el menú y el cliente de consulta y vuelca la config.
    // EN: Update is called every frame from the Present hook (auto-connect, connection
    //     state); Shutdown closes the menu and query client and writes back the config.
    void Update(); // Called every frame from Present hook — handles auto-connect, connection state
    void Shutdown();

    // ES: Chat (historial interno; no se dibuja).
    // EN:
    // Chat
    void AddChatMessage(PlayerID sender, const std::string& message);
    void AddSystemMessage(const std::string& message);

    // ES: Lista de jugadores.
    // EN:
    // Player list
    void AddPlayer(const PlayerInfo& player);
    void RemovePlayer(PlayerID id);
    void UpdatePlayerPing(PlayerID id, uint32_t ping);

    // ES: ¿Captura la entrada? (sí mientras el menú nativo está visible).
    // EN:
    // Input capture state
    bool IsInputCapture() const {
        return m_nativeMenu.IsVisible();
    }

    // ES: Cierra el menú nativo.
    // EN: Closes the native menu.
    void CloseAll() {
        m_nativeMenu.Hide();
    }

    // ES: Acceso al NativeMenu (lo usan los handlers de sus botones).
    // EN:
    // NativeMenu access (used by NativeMenu button handlers)
    NativeMenu& GetNativeMenu() { return m_nativeMenu; }

    // ES: Acceso al ServerQueryClient (lo usa el navegador de servidores del NativeMenu).
    // EN:
    // ServerQueryClient access (used by NativeMenu server browser)
    ServerQueryClient& GetQueryClient() { return m_queryClient; }

    // ES: Auxiliares para que NativeMenu controle el estado de conexión del overlay.
    // EN:
    // Helpers for NativeMenu to drive overlay connection state
    void SetHostingServer(bool hosting) { m_hostingServer = hosting; }
    void SetConnecting(bool connecting) { m_connecting = connecting; }
    void SetAutoConnect(const std::string& ip, uint16_t port);
    void SetConnectionInfo(const std::string& ip, uint16_t port, const std::string& name);
    void SetPlayerName(const std::string& name);

    // ES: Resetear el estado (se llama al desconectar para poder reconectar limpio).
    // EN:
    // Reset overlay state (called on disconnect for clean reconnect)
    void ResetForReconnect();

private:
    // ES: Chat (el timestamp usa m_uptime, que no se incrementa en ningún sitio).
    // EN: (The timestamp uses m_uptime, which is never incremented anywhere.)
    // Chat
    struct ChatEntry {
        PlayerID    sender;
        std::string senderName;
        std::string message;
        float       timestamp;
        bool        isSystem;
    };
    std::deque<ChatEntry> m_chatHistory;
    bool m_chatScrollToBottom = false;

    // ES: Lista de jugadores.
    // EN:
    // Player list
    std::vector<PlayerInfo> m_players;

    // ES: Datos de conexión (buffers de C heredados de la antigua UI ImGui).
    // EN: (C buffers inherited from the old ImGui UI.)
    // Connection
    char m_serverAddress[128] = "127.0.0.1";
    char m_serverPort[8] = "27800";
    char m_playerName[32] = "Player";
    bool m_connecting = false;

    // ES: Config cargada en el primer frame.
    // EN:
    // Config loaded flag
    bool m_firstFrame = true;
    char m_settingsName[32] = "Player";
    bool m_settingsAutoConnect = true;

    // ES: Auto-conexión al cargar la partida.
    // EN:
    // Auto-connect on game load
    bool m_autoConnectPending = false;  // True = connect when game loads
    bool m_autoConnectDone = false;     // True = already attempted

    // ES: Si nosotros lanzamos el proceso del servidor.
    // EN:
    // Hosting
    bool m_hostingServer = false;       // True if we launched the server process

    // ES: Reintentos de conexión (cubren el retraso del mapeo UPnP en el host remoto):
    //     hasta 6 intentos cada 5 s (~30 s).
    // EN:
    // Connection retry (handles UPnP mapping delay on remote host)
    int  m_connectAttempt = 0;
    int  m_maxConnectAttempts = 6;      // ~30 seconds total (5s per attempt)
    bool m_retryPending = false;
    std::chrono::steady_clock::time_point m_retryTime;

    // ES: Estado de detección de partida cargada (miembros en vez de static para que se
    //     reseteen bien).
    // EN:
    // Game load detection state (members instead of statics so they reset properly)
    std::chrono::steady_clock::time_point m_firstUpdateTime;
    std::chrono::steady_clock::time_point m_lastPlayerBaseCheck;
    bool m_startupDelayPassed = false;
    bool m_playerBaseCheckedOnce = false;
    bool m_firstUpdateTimeSet = false;
    int  m_playerBasePollCount = 0;

    // ES: Temporizador de la auto-conexión.
    // EN:
    // Auto-connect timer state (members instead of statics)
    std::chrono::steady_clock::time_point m_gameLoadedTime;
    bool m_gameLoadedTimerStarted = false;

    // ES: Menú nativo MyGUI.
    // EN:
    // Native MyGUI menu
    NativeMenu m_nativeMenu;

    // ES: Cliente de consulta de servidores (host ENet separado para el navegador).
    // EN:
    // Server query client (separate ENet host for browsing)
    ServerQueryClient m_queryClient;

    // ES: General: mutex del chat/jugadores, tiempo de actividad y contador de frames del navegador.
    // EN:
    // General
    std::mutex m_mutex;
    float m_uptime = 0.f;
    int m_browserFrameCounter = 0;

    static constexpr int MAX_CHAT_HISTORY = 100;
};

} // namespace kmp
