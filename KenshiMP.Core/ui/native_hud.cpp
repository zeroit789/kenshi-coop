// ES: Implementación del HUD nativo MyGUI: carga/creación de widgets, refresco periódico
//     de la barra de estado, chat, lista de jugadores y log, y la entrada de chat
//     (mensajes normales al servidor o comandos '/' al CommandRegistry).
// EN: Native MyGUI HUD implementation: widget loading/creation, periodic refresh of the
//     status bar, chat, player list and log, and chat input (plain messages to the
//     server or '/' commands to the CommandRegistry).
#include "native_hud.h"
#include "mygui_bridge.h"
#include "../core.h"
#include "../sys/command_registry.h"
#include "../game/game_types.h"
#include "../game/spawn_manager.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include <spdlog/spdlog.h>
#include <sstream>
#include <Windows.h>

namespace kmp {

// ES: Inicializa el HUD: carga Kenshi_MultiplayerHUD.layout y cachea sus widgets; si
//     falla, crea los widgets por código. Necesita el puente MyGUI ya listo.
// EN: Initializes the HUD: loads Kenshi_MultiplayerHUD.layout and caches its widgets;
//     on failure, creates the widgets in code. Needs the MyGUI bridge to be ready.
bool NativeHud::Init() {
    if (m_initialized) return true;

    auto& bridge = MyGuiBridge::Get();
    if (!bridge.IsReady()) return false;

    bool layoutOk = bridge.LoadLayout("Kenshi_MultiplayerHUD.layout");

    if (layoutOk) {
        if (!CacheWidgets()) {
            spdlog::error("NativeHud: Layout loaded but failed to cache widgets");
            bridge.UnloadLayout("Kenshi_MultiplayerHUD.layout");
            layoutOk = false;
        }
    }

    if (!layoutOk) {
        spdlog::warn("NativeHud: Layout loading failed, trying programmatic widget creation");
        if (!CreateWidgetsFallback()) {
            spdlog::error("NativeHud: Programmatic creation also failed");
            return false;
        }
    }

    m_initialized = true;
    spdlog::info("NativeHud: Initialized with native MyGUI (layout={})", layoutOk ? "file" : "programmatic");
    return true;
}

// ES: Busca por nombre los widgets definidos en el layout. Solo HUDRoot es obligatorio.
// EN: Looks up the layout-defined widgets by name. Only HUDRoot is required.
bool NativeHud::CacheWidgets() {
    auto& bridge = MyGuiBridge::Get();

    m_root = bridge.FindWidget("HUDRoot");
    m_statusBar = bridge.FindWidget("HUDStatusBar");
    m_statusText = bridge.FindWidget("HUDStatusText");
    m_chatPanel = bridge.FindWidget("HUDChatPanel");
    m_chatInput = bridge.FindWidget("HUDChatInput");
    m_playerListPanel = bridge.FindWidget("HUDPlayerList");
    m_playerListTitle = bridge.FindWidget("HUDPlayerListTitle");

    // ES: Panel de log.
    // Log panel
    m_logPanel = bridge.FindWidget("HUDLogPanel");
    m_logTitle = bridge.FindWidget("HUDLogTitle");
    m_logHint = bridge.FindWidget("HUDLogHint");

    for (int i = 0; i < MAX_CHAT_LINES; i++) {
        m_chatLines[i] = bridge.FindWidget("HUDChat" + std::to_string(i));
    }
    for (int i = 0; i < MAX_PLAYER_ROWS; i++) {
        m_playerRows[i] = bridge.FindWidget("HUDPlayer" + std::to_string(i));
    }
    for (int i = 0; i < MAX_LOG_LINES; i++) {
        m_logLines[i] = bridge.FindWidget("HUDLog" + std::to_string(i));
    }

    int found = (m_root ? 1 : 0) + (m_statusBar ? 1 : 0) + (m_statusText ? 1 : 0)
              + (m_chatPanel ? 1 : 0) + (m_chatInput ? 1 : 0) + (m_playerListPanel ? 1 : 0) + (m_logPanel ? 1 : 0);
    for (int i = 0; i < MAX_CHAT_LINES; i++) if (m_chatLines[i]) found++;
    for (int i = 0; i < MAX_PLAYER_ROWS; i++) if (m_playerRows[i]) found++;
    for (int i = 0; i < MAX_LOG_LINES; i++) if (m_logLines[i]) found++;

    spdlog::info("NativeHud: Cached {} widgets (log={}, status={}, chatPanel={}, chat={})",
                 found, m_logPanel != nullptr, m_statusText != nullptr,
                 m_chatPanel != nullptr, m_chatLines[0] != nullptr);
    return m_root != nullptr;
}

// ES: Plan B: crea todos los widgets del HUD por código (coordenadas relativas 0..1).
// EN: Fallback: creates all HUD widgets in code (relative 0..1 coordinates).
bool NativeHud::CreateWidgetsFallback() {
    auto& bridge = MyGuiBridge::Get();

    // ES: Valores de alineación de MyGUI. OJO: no coinciden con el enum MyGUI::Align real
    //     (MYGUI_FLAG(n) = 1<<n: Left=2, Right=4, HStretch=6, Top=8, Bottom=16,
    //     VStretch=24, Stretch=30, Default=10, Center/HCenter/VCenter=0; ver
    //     tools/KenshiLib-reference/.../MyGUI_Align.h). Con estos valores las
    //     alineaciones probablemente no son las que dicen los nombres (sin verificar en juego).
    // EN: MyGUI Align values. NOTE: they do not match the real MyGUI::Align enum
    //     (MYGUI_FLAG(n) = 1<<n: Left=2, Right=4, HStretch=6, Top=8, Bottom=16,
    //     VStretch=24, Stretch=30, Default=10, Center/HCenter/VCenter=0; see
    //     tools/KenshiLib-reference/.../MyGUI_Align.h). With these values the alignments
    //     are probably not what the names say (not verified in game).
    // MyGUI Align values
    constexpr int ALIGN_DEFAULT     = 0;
    constexpr int ALIGN_HCENTER_TOP = 5;  // HCenter|Top
    constexpr int ALIGN_LEFT_BOTTOM = 24; // Left|Bottom
    constexpr int ALIGN_RIGHT_TOP   = 36; // Right|Top
    constexpr int ALIGN_HSTRETCH    = 48; // Left|Right (HStretch)
    constexpr int ALIGN_STRETCH     = 60; // HStretch|VStretch

    // ES: Raíz: contenedor a pantalla completa que no captura el ratón, en la capa "Main".
    // Root — fullscreen non-interactive container
    m_root = bridge.CreateRootWidget("Widget", "PanelEmpty",
                                      0.f, 0.f, 1.f, 1.f, ALIGN_STRETCH, "Main", "HUDRoot");
    if (!m_root) {
        spdlog::error("NativeHud: Failed to create root widget");
        return false;
    }
    bridge.SetProperty(m_root, "NeedMouse", "false");
    bridge.SetProperty(m_root, "Visible", "false");

    // ES: Barra de estado arriba en el centro.
    // Status bar — top center
    m_statusBar = bridge.CreateChildWidget(m_root, "Widget", "PanelEmpty",
                                            0.25f, 0.003f, 0.5f, 0.035f, ALIGN_HCENTER_TOP, "HUDStatusBar");
    if (m_statusBar) {
        bridge.SetProperty(m_statusBar, "NeedMouse", "false");
    }

    m_statusText = bridge.CreateChildWidget(m_statusBar ? m_statusBar : m_root,
                                             "TextBox", "TextBox",
                                             0.02f, 0.05f, 0.96f, 0.9f, ALIGN_STRETCH, "HUDStatusText");
    if (m_statusText) {
        bridge.SetCaption(m_statusText, "KENSHI ONLINE  |  Not Connected  |  F1 = Menu");
        bridge.SetProperty(m_statusText, "TextAlign", "Center VCenter");
        bridge.SetProperty(m_statusText, "NeedMouse", "false");
    }

    // ES: Panel de log a la derecha.
    // Log panel — right side
    m_logPanel = bridge.CreateChildWidget(m_root, "Widget", "PanelEmpty",
                                           0.6f, 0.05f, 0.39f, 0.55f, ALIGN_RIGHT_TOP, "HUDLogPanel");
    if (m_logPanel) {
        bridge.SetProperty(m_logPanel, "NeedMouse", "false");
    }

    m_logTitle = bridge.CreateChildWidget(m_logPanel ? m_logPanel : m_root,
                                           "TextBox", "TextBox",
                                           0.02f, 0.01f, 0.96f, 0.06f, ALIGN_HSTRETCH, "HUDLogTitle");
    if (m_logTitle) {
        bridge.SetCaption(m_logTitle, "KENSHI ONLINE");
        bridge.SetProperty(m_logTitle, "TextAlign", "Center");
        bridge.SetProperty(m_logTitle, "NeedMouse", "false");
    }

    // ES: Líneas del log (20).
    // Log lines (20 lines)
    if (m_logPanel) {
        for (int i = 0; i < MAX_LOG_LINES; i++) {
            float y = 0.08f + i * 0.04f;
            std::string name = "HUDLog" + std::to_string(i);
            m_logLines[i] = bridge.CreateChildWidget(m_logPanel, "TextBox", "TextBox",
                                                      0.02f, y, 0.96f, 0.04f, ALIGN_HSTRETCH, name);
            if (m_logLines[i]) {
                bridge.SetProperty(m_logLines[i], "TextAlign", "Left VCenter");
                bridge.SetProperty(m_logLines[i], "NeedMouse", "false");
            }
        }
    }

    m_logHint = bridge.CreateChildWidget(m_logPanel ? m_logPanel : m_root,
                                          "TextBox", "TextBox",
                                          0.02f, 0.92f, 0.96f, 0.06f, ALIGN_HSTRETCH, "HUDLogHint");
    if (m_logHint) {
        bridge.SetCaption(m_logHint, "Insert = toggle  |  F1 = menu  |  Enter = chat");
        bridge.SetProperty(m_logHint, "TextAlign", "Center");
        bridge.SetProperty(m_logHint, "NeedMouse", "false");
    }

    // ES: Panel de chat abajo a la izquierda.
    // Chat panel — bottom left (semi-transparent background)
    m_chatPanel = bridge.CreateChildWidget(m_root, "Widget", "PanelEmpty",
                                            0.0f, 0.75f, 0.41f, 0.25f, ALIGN_LEFT_BOTTOM, "HUDChatPanel");
    if (m_chatPanel) {
        bridge.SetProperty(m_chatPanel, "NeedMouse", "false");
        bridge.SetProperty(m_chatPanel, "Visible", "false");
    }

    // ES: Líneas de chat dentro del panel.
    // Chat lines — inside chat panel
    void* chatParent = m_chatPanel ? m_chatPanel : m_root;
    for (int i = 0; i < MAX_CHAT_LINES; i++) {
        float y = 0.04f + i * 0.088f;
        std::string name = "HUDChat" + std::to_string(i);
        m_chatLines[i] = bridge.CreateChildWidget(chatParent, "TextBox", "TextBox",
                                                   0.012f, y, 0.976f, 0.088f, ALIGN_HSTRETCH, name);
        if (m_chatLines[i]) {
            bridge.SetProperty(m_chatLines[i], "TextAlign", "Left VCenter");
            bridge.SetProperty(m_chatLines[i], "NeedMouse", "false");
        }
    }

    // ES: Entrada de chat dentro del panel.
    // Chat input — inside chat panel
    m_chatInput = bridge.CreateChildWidget(chatParent, "TextBox", "TextBox",
                                            0.012f, 0.912f, 0.976f, 0.088f, ALIGN_HSTRETCH, "HUDChatInput");
    if (m_chatInput) {
        bridge.SetProperty(m_chatInput, "TextAlign", "Left VCenter");
        bridge.SetProperty(m_chatInput, "Visible", "false");
        bridge.SetProperty(m_chatInput, "NeedMouse", "false");
    }

    // ES: Lista de jugadores arriba a la derecha.
    // Player list — top right
    m_playerListPanel = bridge.CreateChildWidget(m_root, "Widget", "PanelEmpty",
                                                  0.75f, 0.05f, 0.245f, 0.35f, ALIGN_RIGHT_TOP, "HUDPlayerList");
    if (m_playerListPanel) {
        bridge.SetProperty(m_playerListPanel, "Visible", "false");
        bridge.SetProperty(m_playerListPanel, "NeedMouse", "false");
    }

    m_playerListTitle = bridge.CreateChildWidget(m_playerListPanel ? m_playerListPanel : m_root,
                                                  "TextBox", "TextBox",
                                                  0.0f, 0.0f, 1.0f, 0.12f, ALIGN_HSTRETCH, "HUDPlayerListTitle");
    if (m_playerListTitle) {
        bridge.SetCaption(m_playerListTitle, "PLAYERS ONLINE");
        bridge.SetProperty(m_playerListTitle, "TextAlign", "Center");
        bridge.SetProperty(m_playerListTitle, "NeedMouse", "false");
    }

    for (int i = 0; i < MAX_PLAYER_ROWS; i++) {
        float y = 0.14f + i * 0.1f;
        std::string name = "HUDPlayer" + std::to_string(i);
        m_playerRows[i] = bridge.CreateChildWidget(m_playerListPanel ? m_playerListPanel : m_root,
                                                    "TextBox", "TextBox",
                                                    0.02f, y, 0.96f, 0.1f, ALIGN_HSTRETCH, name);
        if (m_playerRows[i]) {
            bridge.SetProperty(m_playerRows[i], "TextAlign", "Left VCenter");
            bridge.SetProperty(m_playerRows[i], "NeedMouse", "false");
        }
    }

    int found = (m_root ? 1 : 0) + (m_statusText ? 1 : 0) + (m_logPanel ? 1 : 0);
    for (int i = 0; i < MAX_CHAT_LINES; i++) if (m_chatLines[i]) found++;
    for (int i = 0; i < MAX_LOG_LINES; i++) if (m_logLines[i]) found++;
    spdlog::info("NativeHud: Programmatic fallback created {} widgets", found);
    return m_root != nullptr;
}

// ES: Oculta el HUD y descarga el layout.
// EN: Hides the HUD and unloads the layout.
void NativeHud::Shutdown() {
    if (!m_initialized) return;
    Hide();
    MyGuiBridge::Get().UnloadLayout("Kenshi_MultiplayerHUD.layout");
    m_initialized = false;
}

// ES: Muestra / oculta el widget raíz del HUD.
// EN: Shows / hides the HUD root widget.
void NativeHud::Show() {
    if (!m_initialized) return;
    MyGuiBridge::Get().SetVisible(m_root, true);
    m_visible = true;
}

void NativeHud::Hide() {
    if (!m_initialized) return;
    MyGuiBridge::Get().SetVisible(m_root, false);
    m_visible = false;
}

// ES: Actualización por frame (hilo de render, desde el hook de Present): inicializa el
//     HUD en cuanto MyGUI está listo (con reintentos espaciados), refresca los paneles
//     cada cierto número de frames y oculta solo el log 15 s después de cargar sin conexión.
// EN: Per-frame update (render thread, from the Present hook): initializes the HUD as
//     soon as MyGUI is ready (with throttled retries), refreshes panels every few frames
//     and auto-hides the log 15 s after loading when not connected.
void NativeHud::Update() {
    auto& core = Core::Get();

    // ES: Intentar inicializar en cuanto el puente MyGUI esté listo (también en el menú principal).
    // Try to init as soon as MyGUI bridge is ready (works on main menu too)
    if (!m_initialized) {
        static int s_initAttempts = 0;
        static auto s_lastAttempt = std::chrono::steady_clock::time_point{};

        // ES: Limitar reintentos: cada 1 s los 10 primeros, luego cada 10 s.
        // Throttle retries: every 1s for first 10 attempts, every 10s after that
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastAttempt);
        int retryIntervalMs = (s_initAttempts < 10) ? 1000 : 10000;
        if (s_initAttempts > 0 && elapsed.count() < retryIntervalMs) return;

        auto& bridge = MyGuiBridge::Get();
        if (bridge.IsReady()) {
            s_lastAttempt = now;
            s_initAttempts++;
            if (!Init()) {
                if (s_initAttempts == 10) {
                    spdlog::warn("NativeHud: 10 init attempts failed — retrying every 10s");
                }
                return;
            }
            Show();
            // ES: Mostrar el panel de log por defecto.
            // Show log panel by default
            if (m_logPanel) {
                MyGuiBridge::Get().SetVisible(m_logPanel, m_showLogPanel);
            }
        } else {
            return;
        }
    }

    if (!m_initialized || !m_visible) return;

    m_frameCounter++;

    // ES: Barra de estado cada 15 frames (~4 veces por segundo a 60 fps).
    // Update status bar every 15 frames (~4 times per second)
    if (m_frameCounter % 15 == 0) {
        UpdateStatusBar();
    }

    // ES: Resto de paneles cada 30 frames (~2 veces por segundo).
    // Update other panels every 30 frames (~2 times per second)
    if (m_frameCounter % 30 == 0) {
        UpdateChatDisplay();
        UpdatePlayerList();
        UpdateLogPanel();
    }

    // ES: Ocultar el log 15 s después de cargar la partida.
    // Auto-hide log panel 15 seconds after game loads
    if (m_showLogPanel && !m_logAutoHideTriggered && core.IsGameLoaded()) {
        if (m_gameLoadedTime.time_since_epoch().count() == 0) {
            m_gameLoadedTime = std::chrono::steady_clock::now();
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_gameLoadedTime);
        if (elapsed.count() >= 15 && !core.IsConnected()) {
            // ES: Solo si no hay conexión (con conexión se mantiene visible).
            // Only auto-hide if not connected (keep visible when things are happening)
            m_showLogPanel = false;
            m_logAutoHideTriggered = true;
            if (m_logPanel) {
                MyGuiBridge::Get().SetVisible(m_logPanel, false);
            }
        }
    }
}

// ES: Compone el texto de la barra de estado: conectado (nombre, jugadores, remotos, ping),
//     partida cargada sin conexión, o cargando.
// EN: Builds the status bar text: connected (name, players, remotes, ping), game loaded
//     but not connected, or loading.
void NativeHud::UpdateStatusBar() {
    if (!m_statusText) return;

    auto& core = Core::Get();
    auto& bridge = MyGuiBridge::Get();

    std::string status = "KENSHI ONLINE";

    if (core.IsConnected()) {
        auto& pc = core.GetPlayerController();
        auto remotePlayers = pc.GetAllRemotePlayers();

        status += "  |  CONNECTED as " + pc.GetLocalPlayerName();
        status += "  |  " + std::to_string(1 + remotePlayers.size()) + " player"
                + (remotePlayers.size() > 0 ? "s" : "") + " online";

        auto& er = core.GetEntityRegistry();
        size_t remoteCount = er.GetRemoteCount();
        if (remoteCount > 0) {
            status += "  |  " + std::to_string(remoteCount) + " remote";
        }

        // ES: Añadir el ping.
        // Append ping
        uint32_t ping = core.GetClient().GetPing();
        status += "  |  " + std::to_string(ping) + "ms";

        bridge.SetCaption(m_statusText, status);
    } else if (core.IsGameLoaded()) {
        status += "  |  NOT CONNECTED  |  F1 = Menu  |  Enter = Chat";
        bridge.SetCaption(m_statusText, status);
    } else {
        status += "  |  Loading...  |  F1 = Menu";
        bridge.SetCaption(m_statusText, status);
    }
}

// ES: Refresca el chat: descarta mensajes viejos, muestra/oculta el panel y rellena las
//     líneas con los últimos mensajes (los de sistema en ámbar).
// EN: Refreshes the chat: drops old messages, shows/hides the panel and fills the lines
//     with the latest messages (system ones in amber).
void NativeHud::UpdateChatDisplay() {
    auto& bridge = MyGuiBridge::Get();
    std::lock_guard lock(m_chatMutex);

    // ES: Quitar los mensajes más antiguos que CHAT_FADE_SECONDS.
    // Prune old messages
    auto now = std::chrono::steady_clock::now();
    while (!m_chatHistory.empty()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - m_chatHistory.front().time).count();
        if (age > CHAT_FADE_SECONDS) {
            m_chatHistory.pop_front();
        } else {
            break;
        }
    }

    // ES: Mostrar el panel solo si hay mensajes o la entrada está abierta.
    // Show/hide chat panel based on whether there are active messages or input is open
    bool hasContent = !m_chatHistory.empty() || m_chatInputActive;
    if (m_chatPanel) {
        bridge.SetVisible(m_chatPanel, hasContent);
    }

    // ES: Rellenar las líneas (lo más reciente abajo).
    // Fill chat lines (most recent at bottom)
    int msgCount = static_cast<int>(m_chatHistory.size());
    int startIdx = (msgCount > MAX_CHAT_LINES) ? (msgCount - MAX_CHAT_LINES) : 0;

    for (int i = 0; i < MAX_CHAT_LINES; i++) {
        if (!m_chatLines[i]) continue;
        int msgIdx = startIdx + i;
        if (msgIdx < msgCount) {
            auto& entry = m_chatHistory[msgIdx];
            std::string display = entry.timestamp + " " + entry.text;
            bridge.SetCaption(m_chatLines[i], display);
            // ES: Mensajes de sistema en ámbar, chat de jugadores en blanco.
            // Color system messages amber, player chat white
            if (entry.isSystem) {
                bridge.SetProperty(m_chatLines[i], "TextColour", "0.9 0.55 0.1");
            } else {
                bridge.SetProperty(m_chatLines[i], "TextColour", "0.9 0.9 0.9");
            }
        } else {
            bridge.SetCaption(m_chatLines[i], "");
        }
    }
}

// ES: Refresca la lista de jugadores: primero el local ("(You)", "[HOST]") y luego los
//     remotos con su estado ([IN WORLD] / [loading]); vacía las filas sobrantes.
// EN: Refreshes the player list: local player first ("(You)", "[HOST]") then remotes with
//     their state ([IN WORLD] / [loading]); clears leftover rows.
void NativeHud::UpdatePlayerList() {
    if (!m_playerListPanel) return;

    auto& bridge = MyGuiBridge::Get();
    auto& core = Core::Get();

    bridge.SetVisible(m_playerListPanel, m_showPlayerList);
    if (!m_showPlayerList) return;

    int row = 0;

    if (core.IsConnected()) {
        auto& pc = core.GetPlayerController();
        if (row < MAX_PLAYER_ROWS && m_playerRows[row]) {
            std::string localLabel = pc.GetLocalPlayerName() + " (You)";
            if (core.IsHost()) {
                localLabel += "  [HOST]";
            }
            bridge.SetCaption(m_playerRows[row], localLabel);
            row++;
        }

        auto remotePlayers = pc.GetAllRemotePlayers();
        for (auto& rp : remotePlayers) {
            if (row >= MAX_PLAYER_ROWS) break;
            if (!m_playerRows[row]) continue;
            std::string info = rp.playerName;
            if (rp.hasSpawnedCharacter) {
                info += "  [IN WORLD]";
            } else {
                info += "  [loading]";
            }
            bridge.SetCaption(m_playerRows[row], info);
            row++;
        }
    }

    for (int i = row; i < MAX_PLAYER_ROWS; i++) {
        if (m_playerRows[i]) bridge.SetCaption(m_playerRows[i], "");
    }
}

// ES: Refresca el panel de log con las últimas MAX_LOG_LINES entradas.
// EN: Refreshes the log panel with the latest MAX_LOG_LINES entries.
void NativeHud::UpdateLogPanel() {
    if (!m_logPanel) return;

    auto& bridge = MyGuiBridge::Get();
    bridge.SetVisible(m_logPanel, m_showLogPanel);
    if (!m_showLogPanel) return;

    std::lock_guard lock(m_logMutex);

    // ES: Mostrar las MAX_LOG_LINES entradas más recientes.
    // Show the most recent MAX_LOG_LINES entries
    int entryCount = static_cast<int>(m_logEntries.size());
    int startIdx = (entryCount > MAX_LOG_LINES) ? (entryCount - MAX_LOG_LINES) : 0;

    for (int i = 0; i < MAX_LOG_LINES; i++) {
        if (!m_logLines[i]) continue;
        int entryIdx = startIdx + i;
        if (entryIdx < entryCount) {
            auto& entry = m_logEntries[entryIdx];
            std::string line = "[" + entry.tag + "] " + entry.message;
            bridge.SetCaption(m_logLines[i], line);
        } else {
            bridge.SetCaption(m_logLines[i], "");
        }
    }
}

// ES: Añade una entrada al log (cualquier hilo) y la escribe también en spdlog y en
//     OutputDebugString.
// EN: Adds a log entry (any thread) and also writes it to spdlog and OutputDebugString.
void NativeHud::LogStep(const std::string& tag, const std::string& message) {
    std::lock_guard lock(m_logMutex);
    LogEntry entry;
    entry.tag = tag;
    entry.message = message;
    entry.time = std::chrono::steady_clock::now();
    m_logEntries.push_back(entry);
    if (m_logEntries.size() > MAX_LOG_ENTRIES) {
        m_logEntries.pop_front();
    }
    // ES: NativeHud es ahora la única vía de log: se registra aquí directamente.
    // NativeHud is now the sole logging pipeline — log directly here
    std::string logLine = "[" + tag + "] " + message;
    spdlog::info("NativeHud: {}", logLine);
    OutputDebugStringA(("KMP: " + logLine + "\n").c_str());
}

// ES: Alterna la lista de jugadores / el panel de log.
// EN: Toggles the player list / the log panel.
void NativeHud::TogglePlayerList() {
    m_showPlayerList = !m_showPlayerList;
    spdlog::info("NativeHud: Player list {}", m_showPlayerList ? "ON" : "OFF");
}

void NativeHud::ToggleLogPanel() {
    m_showLogPanel = !m_showLogPanel;
    if (m_logPanel) {
        MyGuiBridge::Get().SetVisible(m_logPanel, m_showLogPanel);
    }
    spdlog::info("NativeHud: Log panel {}", m_showLogPanel ? "ON" : "OFF");
}

// ES: Marca de hora local "[HH:MM]" para los mensajes de chat.
// EN: Local "[HH:MM]" timestamp for chat messages.
static std::string MakeTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm local;
    localtime_s(&local, &tt);
    char buf[8];
    snprintf(buf, sizeof(buf), "[%02d:%02d]", local.tm_hour, local.tm_min);
    return buf;
}

// ES: Añade un mensaje de chat de un jugador (máx. MAX_CHAT_HISTORY en memoria).
// EN: Adds a player chat message (max MAX_CHAT_HISTORY kept).
void NativeHud::AddChatMessage(const std::string& sender, const std::string& message) {
    std::lock_guard lock(m_chatMutex);
    ChatEntry entry;
    entry.text = sender + ": " + message;
    entry.timestamp = MakeTimestamp();
    entry.isSystem = false;
    entry.time = std::chrono::steady_clock::now();
    m_chatHistory.push_back(entry);
    if (m_chatHistory.size() > MAX_CHAT_HISTORY) {
        m_chatHistory.pop_front();
    }
    spdlog::info("NativeHud: Chat: {}", entry.text);
}

// ES: Añade un mensaje de sistema con prefijo "[System]".
// EN: Adds a system message prefixed with "[System]".
void NativeHud::AddSystemMessage(const std::string& message) {
    std::lock_guard lock(m_chatMutex);
    ChatEntry entry;
    entry.text = "[System] " + message;
    entry.timestamp = MakeTimestamp();
    entry.isSystem = true;
    entry.time = std::chrono::steady_clock::now();
    m_chatHistory.push_back(entry);
    if (m_chatHistory.size() > MAX_CHAT_HISTORY) {
        m_chatHistory.pop_front();
    }
    spdlog::info("NativeHud: System: {}", message);
}

// ES: Abre / cierra la línea de entrada de chat.
// EN: Opens / closes the chat input line.
void NativeHud::OpenChatInput() {
    m_chatInputActive = true;
    m_chatInputText.clear();
    if (m_chatInput) {
        auto& bridge = MyGuiBridge::Get();
        bridge.SetVisible(m_chatInput, true);
        bridge.SetCaption(m_chatInput, "> _");
    }
}

void NativeHud::CloseChatInput() {
    m_chatInputActive = false;
    m_chatInputText.clear();
    if (m_chatInput) {
        auto& bridge = MyGuiBridge::Get();
        bridge.SetVisible(m_chatInput, false);
        bridge.SetCaption(m_chatInput, "");
    }
}

// ES: Añade un carácter ASCII imprimible a la entrada (máx. 200) y la redibuja.
// EN: Appends a printable ASCII character to the input (max 200) and redraws it.
void NativeHud::OnChatChar(wchar_t ch) {
    if (!m_chatInputActive) return;
    if (ch >= 32 && ch < 127 && m_chatInputText.size() < 200) {
        m_chatInputText += static_cast<char>(ch);
        if (m_chatInput) {
            MyGuiBridge::Get().SetCaption(m_chatInput, "> " + m_chatInputText + "_");
        }
    }
}

// ES: Teclas especiales de la entrada: Retroceso borra, Enter envía, Escape cancela.
// EN: Input special keys: Backspace deletes, Enter sends, Escape cancels.
void NativeHud::OnChatKeyDown(int vk) {
    if (!m_chatInputActive) return;

    if (vk == VK_BACK) {
        if (!m_chatInputText.empty()) {
            m_chatInputText.pop_back();
            if (m_chatInput) {
                MyGuiBridge::Get().SetCaption(m_chatInput, "> " + m_chatInputText + "_");
            }
        }
    } else if (vk == VK_RETURN) {
        if (!m_chatInputText.empty()) {
            SendChatMessage();
        }
        CloseChatInput();
    } else if (vk == VK_ESCAPE) {
        CloseChatInput();
    }
}

// ES: Envía lo escrito: si empieza por '/' lo ejecuta como comando y muestra el resultado
//     línea a línea; si no, lo manda al servidor como C2S_ChatMessage y lo muestra en local.
// EN: Sends the typed text: if it starts with '/' it runs it as a command and shows the
//     result line by line; otherwise it sends it to the server as C2S_ChatMessage and
//     shows it locally.
void NativeHud::SendChatMessage() {
    if (m_chatInputText.empty()) return;

    auto& core = Core::Get();

    // ES: Comandos con barra vía CommandRegistry.
    // ── Handle slash commands via CommandRegistry ──
    if (m_chatInputText[0] == '/') {
        std::string result = CommandRegistry::Get().Execute(m_chatInputText);
        if (!result.empty()) {
            // ES: Partir los resultados multilínea en mensajes de sistema separados.
            // Split multi-line results into individual system messages
            std::istringstream stream(result);
            std::string line;
            while (std::getline(stream, line)) {
                if (!line.empty()) {
                    AddSystemMessage(line);
                }
            }
        }
        return;
    }

    if (!core.IsConnected()) {
        AddSystemMessage("Not connected to a server.");
        return;
    }

    // ES: Enviar el chat al servidor (cabecera + U32 senderId + U16 longitud + texto).
    // Send chat to server (protocol: header + U32 senderId + U16 len + raw string)
    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_ChatMessage);
    writer.WriteU32(core.GetLocalPlayerId());
    writer.WriteString(m_chatInputText);
    core.GetClient().SendReliable(writer.Data(), writer.Size());

    // ES: Mostrarlo en local.
    // Show locally
    std::string localName = core.GetPlayerController().GetLocalPlayerName();
    AddChatMessage(localName, m_chatInputText);
}

} // namespace kmp
