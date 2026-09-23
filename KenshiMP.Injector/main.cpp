// ES: Lanzador gráfico de Kenshi-Online (ventana Win32 simple). Pide la ruta del juego,
//     el nombre de jugador y el servidor; al pulsar PLAY copia KenshiMP.Core.dll, registra
//     el plugin de Ogre, instala el .mod, escribe client.json y arranca Kenshi.
// EN: Kenshi-Online graphical launcher (simple Win32 window). Asks for the game path,
//     player name and server; on PLAY it copies KenshiMP.Core.dll, registers the Ogre
//     plugin, installs the .mod, writes client.json and starts Kenshi.
#include "injector.h"
#include "process.h"
#include <Windows.h>
#include <CommCtrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <Shlwapi.h>
#include <string>
#include <sstream>

#pragma comment(lib, "shlwapi.lib")

// ES: Enlaza comctl32 y pide la versión 6 de los Common Controls vía manifiesto
//     (estilo visual moderno de Windows en los controles).
// EN: Links comctl32 and requests Common Controls v6 via the manifest
//     (modern Windows visual style for the controls).
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ES: ── Controles de la ventana (handles globales) ──
// EN: ── Window controls ──
static HWND g_hwnd;
static HWND g_editGamePath;
static HWND g_editServerAddr;
static HWND g_editServerPort;
static HWND g_editPlayerName;
static HWND g_btnPlay;
static HWND g_btnBrowse;
static HWND g_statusText;

// ES: IDs de control (llegan en WM_COMMAND para saber qué botón se pulsó).
// EN: Control IDs (arrive in WM_COMMAND to tell which button was pressed).
#define IDC_GAMEPATH    101
#define IDC_SERVERADDR  102
#define IDC_SERVERPORT  103
#define IDC_PLAYERNAME  104
#define IDC_PLAY        105
#define IDC_BROWSE      106
#define IDC_STATUS      107

// ES: No se usa (la ruta se lee directamente del cuadro de texto).
// EN: Unused (the path is read straight from the edit box).
static std::wstring s_gamePath;

// ES: Muestra un texto en la línea de estado de la ventana.
// EN: Shows a text in the window's status line.
void UpdateStatus(const wchar_t* text) {
    SetWindowTextW(g_statusText, text);
}

// ES: Acción del botón PLAY: valida la ruta, copia la DLL, registra el plugin de Ogre,
//     instala el .mod, escribe client.json y lanza Kenshi. Se ejecuta en el hilo de la UI
//     (la ventana no responde mientras dura).
// EN: PLAY button action: validates the path, copies the DLL, registers the Ogre plugin,
//     installs the .mod, writes client.json and launches Kenshi. Runs on the UI thread
//     (the window is unresponsive while it runs).
void OnPlay() {
    wchar_t gamePath[MAX_PATH], serverAddr[128], serverPort[8], playerName[32];
    GetWindowTextW(g_editGamePath, gamePath, MAX_PATH);
    GetWindowTextW(g_editServerAddr, serverAddr, 128);
    GetWindowTextW(g_editServerPort, serverPort, 8);
    GetWindowTextW(g_editPlayerName, playerName, 32);

    std::wstring gameDir(gamePath);

    // ES: Validar la ruta del juego (debe contener kenshi_x64.exe)
    // EN: Validate game path
    std::wstring exeCheck = gameDir + L"\\kenshi_x64.exe";
    if (!PathFileExistsW(exeCheck.c_str())) {
        UpdateStatus(L"Error: kenshi_x64.exe not found at the specified path");
        return;
    }

    // ES: 1. Copiar KenshiMP.Core.dll a la carpeta de Kenshi
    // EN: 1. Copy KenshiMP.Core.dll to the Kenshi directory
    UpdateStatus(L"Copying KenshiMP.Core.dll...");
    if (!kmp::CopyPluginDll(gameDir)) {
        // ES: Comprobar si ya existe (quizá de una instalación anterior)
        // EN: Check if it already exists (maybe from a previous install)
        std::wstring existingDll = gameDir + L"\\KenshiMP.Core.dll";
        if (!PathFileExistsW(existingDll.c_str())) {
            UpdateStatus(L"Error: KenshiMP.Core.dll not found. Place it next to the injector.");
            return;
        }
        // ES: La DLL ya existe, se continúa
        // EN: DLL already exists, continue
    }

    // ES: 2. Añadir la línea del plugin en Plugins_x64.cfg
    // EN: 2. Install the plugin line in Plugins_x64.cfg
    UpdateStatus(L"Installing Ogre plugin...");
    if (!kmp::InstallOgrePlugin(gameDir)) {
        UpdateStatus(L"Failed to modify Plugins_x64.cfg");
        return;
    }

    // ES: 3. Instalar el .mod del multijugador (plantillas de jugador, facciones, escuadras)
    // EN: 3. Install multiplayer mod file (player templates, factions, squads)
    UpdateStatus(L"Installing multiplayer mod...");
    kmp::InstallModFile(gameDir); // Not fatal if missing — spawn system has fallbacks

    // ES: 4. Escribir la config de conexión (en %APPDATA%/KenshiMP/client.json), pasando
    //     los textos de UTF-16 a UTF-8. El resultado de WriteConnectConfig no se comprueba.
    // EN: 4. Write connection config (writes to %APPDATA%/KenshiMP/client.json)
    //     converting texts from UTF-16 to UTF-8. WriteConnectConfig's result isn't checked.
    char addrA[128], portA[8], nameA[32];
    WideCharToMultiByte(CP_UTF8, 0, serverAddr, -1, addrA, sizeof(addrA), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, serverPort, -1, portA, sizeof(portA), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, playerName, -1, nameA, sizeof(nameA), nullptr, nullptr);
    kmp::WriteConnectConfig(addrA, portA, nameA);

    // ES: 5. Lanzar el juego
    // EN: 5. Launch the game
    UpdateStatus(L"Launching Kenshi via Steam...");
    if (!kmp::LaunchKenshi(gameDir)) {
        UpdateStatus(L"Failed to launch Kenshi");
        return;
    }

    UpdateStatus(L"Kenshi launched! Click OK in Kenshi's settings, then the mod loads automatically.");
}

// ES: Botón "...": abre el diálogo de selección de carpeta y pone la ruta elegida en el cuadro.
// EN: "..." button: opens the folder picker and puts the chosen path in the edit box.
void OnBrowse() {
    wchar_t path[MAX_PATH] = {};
    BROWSEINFOW bi = {};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"Select Kenshi game directory";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        SHGetPathFromIDListW(pidl, path);
        SetWindowTextW(g_editGamePath, path);
        CoTaskMemFree(pidl);
    }
}

// ES: Procedimiento de ventana: despacha los clics de PLAY/Browse y cierra la app al destruir la ventana.
// EN: Window procedure: dispatches PLAY/Browse clicks and quits the app when the window is destroyed.
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_PLAY: OnPlay(); break;
        case IDC_BROWSE: OnBrowse(); break;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ES: Punto de entrada: crea la ventana y sus controles (ruta, nombre, servidor, puerto,
//     PLAY y estado), rellena la ruta detectada de Kenshi y ejecuta el bucle de mensajes.
// EN: Entry point: creates the window and its controls (path, name, server, port,
//     PLAY and status), pre-fills the detected Kenshi path and runs the message loop.
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    InitCommonControls();

    // ES: Registrar la clase de ventana
    // EN: Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.lpszClassName = L"KenshiOnlineLauncher";
    RegisterClassExW(&wc);

    // ES: Crear la ventana principal (tamaño fijo 480x340)
    // EN: Create window
    g_hwnd = CreateWindowExW(0, L"KenshiOnlineLauncher",
        L"Kenshi-Online Launcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 340,
        nullptr, nullptr, hInstance, nullptr);

    // ES: y = coordenada vertical actual del diseño; label() crea una etiqueta estática en esa fila.
    // EN: y = current vertical layout coordinate; label() creates a static label on that row.
    int y = 15;
    auto label = [&](const wchar_t* text, int x, int w) {
        CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                     x, y, w, 20, g_hwnd, nullptr, hInstance, nullptr);
    };

    // ES: Ruta del juego (rellenada con la ruta autodetectada) y botón de examinar
    // EN: Game path
    label(L"Kenshi Path:", 15, 80);
    y += 22;

    std::wstring defaultPath = kmp::FindKenshiPath();
    g_editGamePath = CreateWindowW(L"EDIT", defaultPath.c_str(),
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        15, y, 350, 24, g_hwnd, (HMENU)IDC_GAMEPATH, hInstance, nullptr);
    g_btnBrowse = CreateWindowW(L"BUTTON", L"...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        372, y, 80, 24, g_hwnd, (HMENU)IDC_BROWSE, hInstance, nullptr);
    y += 35;

    // ES: Nombre del jugador
    // EN: Player name
    label(L"Player Name:", 15, 85);
    y += 22;
    g_editPlayerName = CreateWindowW(L"EDIT", L"Player",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        15, y, 435, 24, g_hwnd, (HMENU)IDC_PLAYERNAME, hInstance, nullptr);
    y += 35;

    // ES: Dirección y puerto del servidor (valores por defecto: servidor público y 27800)
    // EN: Server address
    label(L"Server Address:", 15, 100);
    label(L"Port:", 340, 40);
    y += 22;
    g_editServerAddr = CreateWindowW(L"EDIT", L"162.248.94.149",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        15, y, 315, 24, g_hwnd, (HMENU)IDC_SERVERADDR, hInstance, nullptr);
    g_editServerPort = CreateWindowW(L"EDIT", L"27800",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        340, y, 112, 24, g_hwnd, (HMENU)IDC_SERVERPORT, hInstance, nullptr);
    y += 40;

    // ES: Botón PLAY
    // EN: Play button
    g_btnPlay = CreateWindowW(L"BUTTON", L"PLAY",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_CENTER,
        15, y, 435, 40, g_hwnd, (HMENU)IDC_PLAY, hInstance, nullptr);
    y += 50;

    // ES: Línea de estado
    // EN: Status
    g_statusText = CreateWindowW(L"STATIC", L"Ready. Press PLAY to launch Kenshi-Online.",
        WS_CHILD | WS_VISIBLE,
        15, y, 435, 20, g_hwnd, (HMENU)IDC_STATUS, hInstance, nullptr);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    // ES: Bucle de mensajes de Windows hasta que se cierre la ventana
    // EN: Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
