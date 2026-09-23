// ES: HUD nativo del mod dibujado con MyGUI (la UI del propio Kenshi): barra de estado
//     de conexión, panel de log de carga/depuración, chat con entrada de texto y lista
//     de jugadores. Sustituye al antiguo overlay GDI de Win32.
// EN: The mod's native HUD drawn with MyGUI (Kenshi's own UI): connection status bar,
//     loading/debug log panel, chat with text input and player list. Replaces the old
//     Win32 GDI overlay.
#pragma once
#include <string>
#include <deque>
#include <mutex>
#include <chrono>

namespace kmp {

// ES: HUD nativo MyGUI en el juego (sustituye al overlay GDI de Win32). Usa
//     Kenshi_MultiplayerHUD.layout (o crea los widgets por código si falla). Muestra
//     barra de estado, log de depuración/carga, chat y lista de jugadores; visible
//     tanto en el menú principal como en partida.
// EN:
// Native MyGUI in-game HUD — replaces the Win32 GDI overlay.
// Uses Kenshi_MultiplayerHUD.layout for all rendering.
// Shows: connection status bar, debug/loading log, chat, player list.
// Visible on both main menu and in-game.
class NativeHud {
public:
    // ES: Carga el layout y cachea los widgets (o los crea por código). Shutdown los descarga.
    // EN: Loads the layout and caches the widgets (or creates them in code). Shutdown unloads them.
    bool Init();
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    // ES: Llamar cada frame desde el hook de Present (reintenta Init y refresca los paneles).
    // EN:
    // Call every frame from Present hook
    void Update();

    // ES: Mostrar/ocultar todo el HUD.
    // EN:
    // Show/hide the entire HUD
    void Show();
    void Hide();
    bool IsVisible() const { return m_visible; }

    // ES: Alternar paneles.
    // EN:
    // Toggle panels
    void TogglePlayerList();
    void ToggleLogPanel();
    void ToggleDebugInfo() { m_showDebug = !m_showDebug; }

    // ES: Chat: añadir mensaje de un jugador o de sistema.
    // EN:
    // Chat
    void AddChatMessage(const std::string& sender, const std::string& message);
    void AddSystemMessage(const std::string& message);

    // ES: Log de depuración/carga — seguro entre hilos, se puede llamar desde cualquiera.
    //     Sustituye al antiguo HudOverlay::LogStep de GDI.
    // EN:
    // Debug/loading log — thread-safe, called from any thread
    // Replaces the old GDI HudOverlay::LogStep
    void LogStep(const std::string& tag, const std::string& message);

    // ES: Entrada de chat (alimentada desde el WndProc): abrir/cerrar, carácter y tecla.
    // EN:
    // Chat input (WndProc-driven)
    bool IsChatInputActive() const { return m_chatInputActive; }
    void OpenChatInput();
    void CloseChatInput();
    void OnChatChar(wchar_t ch);
    void OnChatKeyDown(int vk);

    // ES: Consultas de visibilidad de los paneles.
    // EN: Panel visibility queries.
    bool IsPlayerListVisible() const { return m_showPlayerList; }
    bool IsDebugVisible() const { return m_showDebug; }
    bool IsLogPanelVisible() const { return m_showLogPanel; }

    // ES: Número de líneas/filas de cada panel (deben coincidir con los widgets del layout).
    // EN: Line/row count of each panel (must match the layout widgets).
    static constexpr int MAX_CHAT_LINES = 10;
    static constexpr int MAX_PLAYER_ROWS = 8;
    static constexpr int MAX_LOG_LINES = 20;

private:
    // ES: Buscar los widgets del layout por nombre / crearlos por código si el layout falla
    //     / refrescar cada panel / enviar el mensaje de chat escrito.
    // EN: Look up layout widgets by name / create them in code if the layout fails
    //     / refresh each panel / send the typed chat message.
    bool CacheWidgets();
    bool CreateWidgetsFallback();  // Programmatic creation when layout fails
    void UpdateStatusBar();
    void UpdateChatDisplay();
    void UpdatePlayerList();
    void UpdateLogPanel();
    void SendChatMessage();

    // ES: Punteros a widgets (MyGUI::Widget* opacos).
    // EN:
    // Widget pointers
    void* m_root = nullptr;
    void* m_statusBar = nullptr;
    void* m_statusText = nullptr;
    void* m_chatPanel = nullptr;
    void* m_chatLines[MAX_CHAT_LINES] = {};
    void* m_chatInput = nullptr;
    void* m_playerListPanel = nullptr;
    void* m_playerListTitle = nullptr;
    void* m_playerRows[MAX_PLAYER_ROWS] = {};

    // ES: Widgets del panel de log.
    // EN:
    // Log panel widgets
    void* m_logPanel = nullptr;
    void* m_logTitle = nullptr;
    void* m_logLines[MAX_LOG_LINES] = {};
    void* m_logHint = nullptr;

    // ES: Estado.
    // EN:
    // State
    bool m_initialized = false;
    bool m_visible = false;
    bool m_showPlayerList = false;
    bool m_showDebug = false;
    bool m_showLogPanel = true;  // Visible by default during loading
    bool m_chatInputActive = false;
    std::string m_chatInputText;
    int m_frameCounter = 0;

    // ES: Historial de mensajes de chat (se desvanecen a los CHAT_FADE_SECONDS segundos).
    // EN:
    // Chat message history
    struct ChatEntry {
        std::string text;
        std::string timestamp;  // "[HH:MM]" formatted
        bool isSystem;
        std::chrono::steady_clock::time_point time;
    };
    std::deque<ChatEntry> m_chatHistory;
    static constexpr int MAX_CHAT_HISTORY = 50;
    static constexpr int CHAT_FADE_SECONDS = 30;

    // ES: Entradas del log de depuración/carga.
    // EN:
    // Debug/loading log entries
    struct LogEntry {
        std::string tag;
        std::string message;
        std::chrono::steady_clock::time_point time;
    };
    std::deque<LogEntry> m_logEntries;
    static constexpr int MAX_LOG_ENTRIES = 100;

    // ES: Mutex del chat y del log (se escriben desde cualquier hilo).
    // EN: Chat and log mutexes (written from any thread).
    std::mutex m_chatMutex;
    std::mutex m_logMutex;

    // ES: Ocultar automáticamente el panel de log tras cargar la partida.
    // EN:
    // Auto-hide log panel after game loads
    bool m_logAutoHideTriggered = false;
    std::chrono::steady_clock::time_point m_gameLoadedTime;
};

} // namespace kmp
