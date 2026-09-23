// ES: Menú nativo de multijugador hecho con MyGUI (Kenshi_MultiplayerPanel.layout):
//     botones Host/Join/Settings/Server Browser, subpanel de conexión, ajustes y
//     navegador de servidores. Los clics y el teclado llegan desde el WndProc y se
//     detectan por coordenadas de pantalla (no por eventos de MyGUI).
// EN: Native multiplayer menu built with MyGUI (Kenshi_MultiplayerPanel.layout):
//     Host/Join/Settings/Server Browser buttons, join sub-panel, settings and server
//     browser. Clicks and keyboard come from the WndProc and are detected by screen
//     coordinates (not through MyGUI events).
#pragma once
#include <string>

namespace kmp {

// ES: Gestiona el panel nativo MyGUI de multijugador (Kenshi_MultiplayerPanel.layout).
//     Pretende ser indistinguible de la UI nativa de Kenshi (mismas fuentes, skins y estilo).
// EN:
// Manages the native MyGUI multiplayer panel (Kenshi_MultiplayerPanel.layout).
// Looks indistinguishable from native Kenshi UI — same fonts, skins, styling.
class NativeMenu {
public:
    static constexpr int MAX_SERVER_ROWS = 8;

    // ES: Inicializa: prepara el puente MyGUI, carga el layout y cachea los widgets.
    //     Llamar cuando MyGUI ya esté inicializado (tras el menú principal de Kenshi).
    // EN:
    // Initialize: resolve MyGUI bridge, load layout, cache widget pointers.
    // Call after MyGUI is initialized (i.e., after Kenshi's main menu is up).
    bool Init();
    bool IsInitialized() const { return m_initialized; }

    // ES: Mostrar/ocultar todo el panel.
    // EN:
    // Show/hide the entire panel
    void Show();
    void Hide();
    bool IsVisible() const { return m_visible; }

    // ES: Se llama cada frame (ahora no hace nada; la entrada va por OnClick/WndProc).
    // EN:
    // Called each frame to detect button clicks and manage state
    void Update();

    // ES: Procesa un clic en coordenadas de pantalla (desde el WndProc, sin ImGui).
    // EN:
    // Handle a mouse click at screen coordinates (from WndProc, no ImGui needed)
    void OnClick(int screenX, int screenY);

    // ES: Teclado para los campos de texto (WM_CHAR / WM_KEYDOWN del WndProc).
    // EN:
    // Handle keyboard input for EditBox fields (from WndProc WM_CHAR/WM_KEYDOWN)
    void OnChar(wchar_t ch);
    void OnKeyDown(int vk);
    bool HasActiveEditBox() const { return m_activeField != ActiveField::None; }

    // ES: Cierre: descarga el layout.
    // EN:
    // Shutdown: unload layout
    void Shutdown();

    // ES: Lee lo que ha escrito el usuario en los campos (IP, puerto, nombre).
    // EN:
    // Read user input from EditBoxes
    std::string GetServerIP();
    std::string GetServerPort();
    std::string GetPlayerName();

    // ES: Pone el texto de estado del panel de conexión.
    // EN:
    // Set status text (shown in join panel)
    void SetStatus(const std::string& text);

    // ES: Acceso a los widgets de las filas del navegador (los usa Overlay para refrescarlas).
    // EN:
    // Server browser widget accessors (used by Overlay to update display)
    void* GetServerNameWidget(int row) { return (row >= 0 && row < MAX_SERVER_ROWS) ? m_serverNameTexts[row] : nullptr; }
    void* GetServerInfoWidget(int row) { return (row >= 0 && row < MAX_SERVER_ROWS) ? m_serverInfoTexts[row] : nullptr; }

    // ES: Subpaneles y cambio de subpanel visible.
    // EN:
    // Sub-panel visibility
    enum class Panel { MainButtons, Join, Settings, ServerBrowser };
    void ShowPanel(Panel panel);

private:
    // ES: Acciones de cada botón.
    // EN:
    // Button action handlers
    void OnHostClicked();
    void OnConnectClicked();
    void OnSettingsSaved();
    void OnServerBrowserClicked();
    void OnRefreshServersClicked();

    // ES: Resuelve todos los punteros a widgets tras cargar el layout.
    // EN:
    // Attempt to resolve all widget pointers after layout is loaded
    bool CacheWidgets();

    // ES: Punteros a widgets (opacos, son MyGUI::Widget*).
    // EN:
    // Widget pointers (opaque — these are MyGUI::Widget*)
    void* m_root = nullptr;               // Root fullscreen overlay
    void* m_multiplayerPanel = nullptr;    // The floating panel

    // ES: Botones principales.
    // EN:
    // Main buttons
    void* m_hostButton = nullptr;
    void* m_joinButton = nullptr;
    void* m_settingsButton = nullptr;
    void* m_browserButton = nullptr;
    void* m_backButton = nullptr;

    // ES: Subpanel de conexión (Join).
    // EN:
    // Join sub-panel
    void* m_joinPanel = nullptr;
    void* m_serverIPEdit = nullptr;
    void* m_serverPortEdit = nullptr;
    void* m_playerNameEdit = nullptr;
    void* m_connectButton = nullptr;
    void* m_joinBackButton = nullptr;
    void* m_statusText = nullptr;

    // ES: Subpanel de ajustes.
    // EN:
    // Settings sub-panel
    void* m_settingsPanel = nullptr;
    void* m_settingsNameEdit = nullptr;
    void* m_autoConnectTick = nullptr;
    void* m_settingsBackButton = nullptr;

    // ES: Subpanel del navegador de servidores y sus filas (hasta MAX_SERVER_ROWS).
    // EN:
    // Server Browser sub-panel
    void* m_browserPanel = nullptr;
    void* m_browserTitle = nullptr;
    void* m_browserRefreshButton = nullptr;
    void* m_browserBackButton = nullptr;
    void* m_browserStatusText = nullptr;
    // Server list rows (up to MAX_SERVER_ROWS)
    void* m_serverNameTexts[MAX_SERVER_ROWS] = {};
    void* m_serverInfoTexts[MAX_SERVER_ROWS] = {};

    // ES: Texto de créditos.
    // EN:
    // Credit
    void* m_creditText = nullptr;

    // ES: Campo de texto activo para la entrada de teclado.
    // EN:
    // Active EditBox tracking for keyboard input
    enum class ActiveField { None, IP, Port, Name, SettingsName };
    ActiveField m_activeField = ActiveField::None;

    // ES: Estado.
    // EN:
    // State
    bool m_initialized = false;
    bool m_visible = false;
    Panel m_currentPanel = Panel::MainButtons;
    int m_selectedServerRow = -1;

    // ES: Detección de clics por coordenadas: son los position_real del layout (fracciones
    //     0..1) relativos al panel.
    // EN:
    // Button click detection via screen-space coordinates
    // These are the position_real values from the layout, relative to the panel
    struct ButtonRect {
        float relX, relY, relW, relH;  // Relative to MultiplayerPanel
    };

    // ES: Declarada pero no se ve implementada en native_menu.cpp (probablemente código muerto).
    // EN: Declared but no implementation is visible in native_menu.cpp (probably dead code).
    bool IsButtonClicked(void* button, const ButtonRect& rect);

    // ES: Posición/tamaño del panel en pantalla (fracciones 0..1, de Root > MultiplayerPanel
    //     en el layout). Si se cambia el layout hay que cambiar estas constantes.
    // EN: (If the layout changes, these constants must be updated too.)
    // Panel position_real in screen space (from Root > MultiplayerPanel)
    static constexpr float PANEL_X = 0.520312f;
    static constexpr float PANEL_Y = 0.138889f;
    static constexpr float PANEL_W = 0.364062f;
    static constexpr float PANEL_H = 0.768519f;

    // ES: Rectángulos de los botones principales (relativos al panel).
    // EN:
    // Main button rects (relative to panel)
    static constexpr ButtonRect HOST_BTN     = {0.1f, 0.13f, 0.8f, 0.09f};
    static constexpr ButtonRect JOIN_BTN     = {0.1f, 0.25f, 0.8f, 0.09f};
    static constexpr ButtonRect SETTINGS_BTN = {0.1f, 0.37f, 0.8f, 0.09f};
    static constexpr ButtonRect BROWSER_BTN  = {0.1f, 0.49f, 0.8f, 0.09f};
    static constexpr ButtonRect BACK_BTN     = {0.1f, 0.61f, 0.8f, 0.09f};

    // ES: Subpanel Join: situado en (0.05, 0.13, 0.9, 0.75) dentro del panel.
    // EN:
    // Join panel buttons (relative to join panel which is at 0.05, 0.13, 0.9, 0.75 within panel)
    static constexpr float JOIN_PANEL_X = 0.05f;
    static constexpr float JOIN_PANEL_Y = 0.13f;
    static constexpr float JOIN_PANEL_W = 0.9f;
    static constexpr float JOIN_PANEL_H = 0.75f;

    static constexpr ButtonRect CONNECT_BTN   = {0.05f, 0.5f, 0.42f, 0.14f};
    static constexpr ButtonRect JOIN_BACK_BTN = {0.53f, 0.5f, 0.42f, 0.14f};

    // ES: Rectángulos de los campos de texto del subpanel Join (clic = foco).
    // EN:
    // EditBox rects within join sub-panel (for click-to-focus detection)
    static constexpr ButtonRect IP_EDIT_RECT   = {0.32f, 0.0f, 0.66f, 0.1f};
    static constexpr ButtonRect PORT_EDIT_RECT = {0.32f, 0.14f, 0.3f, 0.1f};
    static constexpr ButtonRect NAME_EDIT_RECT = {0.32f, 0.28f, 0.66f, 0.1f};

    // ES: Campo de texto del subpanel de ajustes.
    // EN:
    // EditBox rect within settings sub-panel
    static constexpr ButtonRect SETTINGS_NAME_EDIT_RECT = {0.42f, 0.0f, 0.56f, 0.1f};

    // ES: Botones del subpanel de ajustes (mismo origen que el de Join).
    // EN:
    // Settings panel buttons (relative to settings panel same origin as join panel)
    static constexpr ButtonRect SETTINGS_BACK_BTN = {0.25f, 0.5f, 0.5f, 0.14f};
    static constexpr ButtonRect AUTO_CONNECT_TICK = {0.42f, 0.15f, 0.06f, 0.08f};

    // ES: Navegador de servidores (mismo origen que Join); filas de ~0.085 desde Y=0.08.
    // EN:
    // Server browser panel (same sub-panel origin as join panel)
    static constexpr ButtonRect BROWSER_REFRESH_BTN = {0.05f, 0.85f, 0.42f, 0.12f};
    static constexpr ButtonRect BROWSER_BACK_BTN    = {0.53f, 0.85f, 0.42f, 0.12f};
    // Server rows (8 rows, each ~0.08 height, starting at Y=0.08)
    static constexpr float SERVER_ROW_Y_START = 0.08f;
    static constexpr float SERVER_ROW_HEIGHT  = 0.085f;

    // ES: Declarada pero no se ve implementada (OnClick usa una lambda equivalente).
    // EN: Declared but no implementation is visible (OnClick uses an equivalent lambda).
    // Convert sub-panel-relative rect to screen coords and test click
    bool IsSubPanelButtonClicked(const ButtonRect& btnInSubPanel,
                                  float subPanelX, float subPanelY,
                                  float subPanelW, float subPanelH);

    // ES: Estado del check de auto-conexión (el TickBox de MyGUI no guarda estado propio aquí).
    // EN:
    // Auto-connect tick state (MyGUI TickBox doesn't have built-in state)
    bool m_autoConnectChecked = true;

    // ES: Valores guardados: respaldo si GetCaption falla (desajuste de SSO, etc.). Son la
    //     fuente real de lo tecleado, porque los EditBox son de solo lectura.
    // EN: (They are the real source of typed text, since the EditBoxes are read-only.)
    // Stored values — fallback if GetCaption fails (SSO mismatch, etc.)
    std::string m_storedIP = "127.0.0.1";
    std::string m_storedPort = "27800";
    std::string m_storedName = "Player";
};

} // namespace kmp
