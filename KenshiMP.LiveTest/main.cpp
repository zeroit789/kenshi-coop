// ES: KenshiMP.LiveTest — test de integración completo con dos jugadores.
//     Modos:
//       Simple: KenshiMP.LiveTest.exe [nombreJugador] [ipServidor] [puertoServidor]
//       Dual:   KenshiMP.LiveTest.exe --dual [nombreJugador]
//     En modo dual lanza:
//       1. KenshiMP.Server (servidor dedicado en 127.0.0.1:27800)
//       2. KenshiMP.TestClient (Jugador 2 automático — camina y envía posiciones)
//       3. Kenshi vía Steam (Jugador 1 — cliente real del juego con la DLL del mod)
//     Vigila el log de Kenshi y la salida estándar del TestClient buscando hitos:
//       - Los dos jugadores conectados
//       - Los dos jugadores con entidades creadas
//       - Visibilidad cruzada de entidades (P1 ve a P2 y P2 ve a P1)
//       - Sincronización de posiciones en ambos sentidos
//       - Autoridad bien asignada (cada jugador solo es dueño de sus entidades)
//     Los hitos se detectan buscando textos concretos en los logs, así que dependen de
//     que los mensajes de log del Core/TestClient no cambien.
// EN: KenshiMP.LiveTest — Full dual-player integration test
//     Milestones are detected by searching for specific text in the logs, so they depend
//     on the Core/TestClient log messages staying unchanged.
//
// Modes:
//   Single: KenshiMP.LiveTest.exe [playerName] [serverIP] [serverPort]
//   Dual:   KenshiMP.LiveTest.exe --dual [playerName]
//
// In dual mode, launches:
//   1. KenshiMP.Server (dedicated server on 127.0.0.1:27800)
//   2. KenshiMP.TestClient (automated Player 2 — walks around, sends positions)
//   3. Kenshi via Steam (Player 1 — real game client with mod DLL)
//
// Monitors both Kenshi log + TestClient stdout for milestones:
//   - Both players connected
//   - Both players spawned entities
//   - Cross-player entity visibility (P1 sees P2, P2 sees P1)
//   - Position sync in both directions
//   - Authority correctly assigned (each player owns only their own entities)

#include <Windows.h>
#include <Shlwapi.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cstdio>
#include <filesystem>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

// ═══════════════════════════════════════════════════════════════════════════
//  CONSOLE COLORS
// ═══════════════════════════════════════════════════════════════════════════
// ES: Colores de consola: atributos de texto de Win32 y funciones de impresión con
//     prefijo de color (OK, WARN, FAIL, INFO, STEP, P1, P2).
// EN: Console colors: Win32 text attributes and print helpers with a colored
//     prefix (OK, WARN, FAIL, INFO, STEP, P1, P2).

static HANDLE g_hConsole = INVALID_HANDLE_VALUE;

enum Color { WHITE = 7, GREEN = 10, YELLOW = 14, RED = 12, CYAN = 11, GRAY = 8, MAGENTA = 13 };

static void SetColor(Color c) {
    if (g_hConsole != INVALID_HANDLE_VALUE)
        SetConsoleTextAttribute(g_hConsole, static_cast<WORD>(c));
}

static void Print(Color c, const char* prefix, const std::string& msg) {
    SetColor(c);
    printf("[%s] ", prefix);
    SetColor(WHITE);
    printf("%s\n", msg.c_str());
}

static void PrintOK(const std::string& msg)   { Print(GREEN,   " OK ", msg); }
static void PrintWarn(const std::string& msg)  { Print(YELLOW,  "WARN", msg); }
static void PrintErr(const std::string& msg)   { Print(RED,     "FAIL", msg); }
static void PrintInfo(const std::string& msg)  { Print(CYAN,    "INFO", msg); }
static void PrintStep(const std::string& msg)  { Print(WHITE,   "STEP", msg); }
static void PrintP1(const std::string& msg)    { Print(GREEN,   " P1 ", msg); }
static void PrintP2(const std::string& msg)    { Print(MAGENTA, " P2 ", msg); }

// ═══════════════════════════════════════════════════════════════════════════
//  UTILITY: Find Kenshi install path
// ═══════════════════════════════════════════════════════════════════════════
// ES: Localiza Kenshi (registro de Steam o Program Files). Copia de la función del
//     Injector (process.cpp), con el mismo doble RegCloseKey.
// EN: Locates Kenshi (Steam registry or Program Files). Copy of the Injector's
//     function (process.cpp), with the same double RegCloseKey.

static std::wstring FindKenshiPath() {
    // ES: Probar el registro de Steam
    // EN: Try Steam registry
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t steamPath[MAX_PATH] = {};
        DWORD size = sizeof(steamPath);
        if (RegQueryValueExW(hKey, L"InstallPath", nullptr, nullptr,
                            reinterpret_cast<LPBYTE>(steamPath), &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            std::wstring path = std::wstring(steamPath) + L"\\steamapps\\common\\Kenshi";
            if (PathFileExistsW((path + L"\\kenshi_x64.exe").c_str())) return path;
        }
        RegCloseKey(hKey);
    }

    std::wstring defaultPath = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Kenshi";
    if (PathFileExistsW((defaultPath + L"\\kenshi_x64.exe").c_str())) return defaultPath;
    return L"";
}

// ═══════════════════════════════════════════════════════════════════════════
//  UTILITY: Install Ogre plugin
// ═══════════════════════════════════════════════════════════════════════════
// ES: Añade "Plugin=KenshiMP.Core" a Plugins_x64.cfg si falta, para que Ogre cargue la
//     DLL del mod al arrancar Kenshi (copia de la lógica del Injector).
// EN: Adds "Plugin=KenshiMP.Core" to Plugins_x64.cfg if missing, so Ogre loads the mod
//     DLL when Kenshi starts (copy of the Injector's logic).

static bool InstallOgrePlugin(const std::wstring& gamePath) {
    std::wstring cfgPath = gamePath + L"\\Plugins_x64.cfg";
    std::ifstream inFile(cfgPath);
    if (!inFile.is_open()) return false;

    std::vector<std::string> lines;
    std::string line;
    bool found = false;
    while (std::getline(inFile, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "Plugin=KenshiMP.Core") found = true;
        lines.push_back(line);
    }
    inFile.close();

    if (found) return true;

    lines.push_back("Plugin=KenshiMP.Core");
    std::ofstream outFile(cfgPath);
    if (!outFile.is_open()) return false;
    for (size_t i = 0; i < lines.size(); i++) {
        outFile << lines[i];
        if (i + 1 < lines.size()) outFile << "\n";
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  UTILITY: Write client.json for auto-connect
// ═══════════════════════════════════════════════════════════════════════════
// ES: Escribe %APPDATA%\KenshiMP\client.json con autoConnect=true para que el Core se
//     conecte solo al cargar la partida. Ojo: el JSON se monta a mano sin escapar, así que
//     un nombre con comillas o barras invertidas produciría un JSON inválido.
// EN: Writes %APPDATA%\KenshiMP\client.json with autoConnect=true so Core connects by
//     itself once the game loads. Note: the JSON is built by hand without escaping, so a
//     name with quotes or backslashes would produce invalid JSON.

static bool WriteClientConfig(const std::string& playerName, const std::string& serverIP, int port) {
    char appData[MAX_PATH];
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) return false;

    std::string dir = std::string(appData) + "\\KenshiMP";
    CreateDirectoryA(dir.c_str(), nullptr);

    std::string path = dir + "\\client.json";
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "{\n";
    file << "  \"playerName\": \"" << playerName << "\",\n";
    file << "  \"lastServer\": \"" << serverIP << "\",\n";
    file << "  \"lastPort\": " << port << ",\n";
    file << "  \"autoConnect\": true,\n";
    file << "  \"overlayScale\": 1.0\n";
    file << "}\n";

    PrintOK("client.json written: name='" + playerName + "' server=" + serverIP + ":" + std::to_string(port) + " autoConnect=true");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  UTILITY: Copy DLL to game directory
// ═══════════════════════════════════════════════════════════════════════════
// ES: Copia KenshiMP.Core.dll a la carpeta del juego desde junto al exe o desde las
//     salidas Debug/Release; si no puede, acepta una DLL que ya estuviera allí.
// EN: Copies KenshiMP.Core.dll into the game folder from next to the exe or from the
//     Debug/Release outputs; if it can't, accepts a DLL already present there.

static bool CopyDllToGame(const std::wstring& gamePath) {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);

    std::wstring candidates[] = {
        exeDir + L"\\KenshiMP.Core.dll",
        gamePath + L"\\KenshiMP\\build\\bin\\Debug\\KenshiMP.Core.dll",
        gamePath + L"\\KenshiMP\\build\\bin\\Release\\KenshiMP.Core.dll",
    };

    std::wstring dst = gamePath + L"\\KenshiMP.Core.dll";

    for (auto& src : candidates) {
        if (PathFileExistsW(src.c_str())) {
            if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
                PrintOK("KenshiMP.Core.dll copied to game directory");
                return true;
            }
        }
    }

    if (PathFileExistsW(dst.c_str())) {
        PrintOK("KenshiMP.Core.dll already in game directory");
        return true;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  UTILITY: Check if a process is running
// ═══════════════════════════════════════════════════════════════════════════
// ES: Recorre la lista de procesos (Toolhelp32) y dice si alguno tiene ese nombre de exe
//     (comparación sin distinguir mayúsculas).
// EN: Walks the process list (Toolhelp32) and reports whether any has that exe name
//     (case-insensitive comparison).

static bool IsProcessRunning(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// ═══════════════════════════════════════════════════════════════════════════
//  UTILITY: Find exe path (server, test client, etc.)
// ═══════════════════════════════════════════════════════════════════════════
// ES: Busca un ejecutable del proyecto junto a este exe, en las salidas Debug/Release o en
//     la carpeta del juego. Devuelve "" si no lo encuentra.
// EN: Looks for a project executable next to this exe, in the Debug/Release outputs or in
//     the game folder. Returns "" if not found.

static std::wstring FindExe(const std::wstring& gamePath, const wchar_t* exeName) {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);

    std::wstring candidates[] = {
        exeDir + L"\\" + exeName,
        gamePath + L"\\KenshiMP\\build\\bin\\Debug\\" + exeName,
        gamePath + L"\\KenshiMP\\build\\bin\\Release\\" + exeName,
        gamePath + L"\\" + exeName,
    };

    for (auto& c : candidates) {
        if (PathFileExistsW(c.c_str())) return c;
    }
    return L"";
}

// ═══════════════════════════════════════════════════════════════════════════
//  UTILITY: Launch server process
// ═══════════════════════════════════════════════════════════════════════════
// ES: Arranca KenshiMP.Server.exe en una consola nueva y devuelve el handle del proceso
//     (INVALID_HANDLE_VALUE si falla). El directorio de trabajo es el heredado de este proceso.
// EN: Starts KenshiMP.Server.exe in a new console and returns the process handle
//     (INVALID_HANDLE_VALUE on failure). The working directory is inherited from this process.

static HANDLE LaunchServer(const std::wstring& gamePath) {
    std::wstring serverExe = FindExe(gamePath, L"KenshiMP.Server.exe");
    if (serverExe.empty()) {
        PrintErr("KenshiMP.Server.exe not found");
        return INVALID_HANDLE_VALUE;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(serverExe.c_str(), nullptr, nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        PrintErr("Failed to launch server (error " + std::to_string(GetLastError()) + ")");
        return INVALID_HANDLE_VALUE;
    }

    CloseHandle(pi.hThread);
    PrintOK("Server launched (PID " + std::to_string(pi.dwProcessId) + ")");
    return pi.hProcess;
}

// ═══════════════════════════════════════════════════════════════════════════
//  UTILITY: Launch TestClient as Player 2 (with output pipe)
// ═══════════════════════════════════════════════════════════════════════════
// ES: Proceso hijo con su stdout/stderr redirigidos a una tubería que leemos nosotros.
// EN: Child process with its stdout/stderr redirected to a pipe we read from.

struct PipedProcess {
    HANDLE hProcess = INVALID_HANDLE_VALUE;
    HANDLE hReadPipe = INVALID_HANDLE_VALUE;
    DWORD  pid = 0;
};

// ES: Lanza KenshiMP.TestClient.exe <ip> <puerto> <nombre> como Jugador 2 capturando su salida.
//     Ojo: la conversión string→wstring es byte a byte (solo vale para ASCII).
// EN: Launches KenshiMP.TestClient.exe <ip> <port> <name> as Player 2, capturing its output.
//     Note: the string→wstring conversion is byte by byte (ASCII only).
static PipedProcess LaunchTestClient(const std::wstring& gamePath,
                                      const std::string& serverIP, int port,
                                      const std::string& playerName) {
    PipedProcess result;

    std::wstring tcExe = FindExe(gamePath, L"KenshiMP.TestClient.exe");
    if (tcExe.empty()) {
        PrintErr("KenshiMP.TestClient.exe not found");
        return result;
    }

    // ES: Crear la tubería para leer la salida estándar del TestClient
    // EN: Create pipe for reading TestClient stdout
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        PrintErr("Failed to create pipe for TestClient");
        return result;
    }
    // ES: El extremo de lectura no lo hereda el hijo
    // EN: Don't inherit the read end
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    // ES: Montar la línea de comandos: TestClient.exe <ip> <puerto> <nombre>
    // EN: Build command line: TestClient.exe <ip> <port> <name>
    std::wstring cmdLine = L"\"" + tcExe + L"\" " +
        std::wstring(serverIP.begin(), serverIP.end()) + L" " +
        std::to_wstring(port) + L" " +
        std::wstring(playerName.begin(), playerName.end());

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, const_cast<wchar_t*>(cmdLine.c_str()),
                        nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        PrintErr("Failed to launch TestClient (error " + std::to_string(GetLastError()) + ")");
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return result;
    }

    CloseHandle(pi.hThread);
    // ES: Cerrar el extremo de escritura en el padre (si no, ReadFile nunca vería fin de datos)
    // EN: Close the write end in the parent (otherwise ReadFile would never see end of data)
    CloseHandle(hWritePipe); // Close write end in parent

    result.hProcess = pi.hProcess;
    result.hReadPipe = hReadPipe;
    result.pid = pi.dwProcessId;
    PrintOK("TestClient launched as '" + playerName + "' (PID " + std::to_string(pi.dwProcessId) + ")");
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  UTILITY: Launch Kenshi via Steam
// ═══════════════════════════════════════════════════════════════════════════
// ES: Pide a Steam arrancar Kenshi (App ID 233860). True si ShellExecute tuvo éxito (> 32).
// EN: Asks Steam to start Kenshi (App ID 233860). True if ShellExecute succeeded (> 32).

static bool LaunchKenshi() {
    HINSTANCE result = ShellExecuteW(nullptr, L"open",
        L"steam://rungameid/233860", nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
}

// ═══════════════════════════════════════════════════════════════════════════
//  UTILITY: Find Kenshi process PID (waits for it)
// ═══════════════════════════════════════════════════════════════════════════
// ES: Espera (sondeando cada segundo) a que aparezca kenshi_x64.exe y devuelve su PID,
//     o 0 si pasa timeoutSeconds.
// EN: Waits (polling every second) for kenshi_x64.exe to appear and returns its PID,
//     or 0 after timeoutSeconds.

static DWORD FindKenshiPID(int timeoutSeconds = 60) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = {};
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"kenshi_x64.exe") == 0) {
                        CloseHandle(snap);
                        return pe.th32ProcessID;
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() >= timeoutSeconds) return 0;

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  MILESTONES: Player 1 (Kenshi game client, from log file)
// ═══════════════════════════════════════════════════════════════════════════
// ES: Hito del test: nombre visible, texto a buscar en el log/salida y si ya se alcanzó.
// EN: Test milestone: display name, text to search in the log/output and whether it was hit.

struct Milestone {
    const char* name;
    const char* searchString;
    bool        hit = false;
};

// ES: Hitos del Jugador 1 (Kenshi con el mod), buscados en el log del Core.
//     Ojo: el dashboard y el informe final usan índices fijos (p.ej. [10] = "Remote Entity
//     Spawn"); si se reordena esta lista hay que actualizarlos. "Player '" es muy genérico.
// EN: Player 1 milestones (Kenshi with the mod), searched in the Core log.
//     Note: the dashboard and final report use fixed indices (e.g. [10] = "Remote Entity
//     Spawn"); reordering this list requires updating them. "Player '" is very generic.
static Milestone g_p1Milestones[] = {
    {"P1: DLL Loaded",          "=== Kenshi-Online v"},
    {"P1: Config Loaded",       "Config loaded"},
    {"P1: Scanner OK",          "Pattern scanner resolved"},
    {"P1: Hooks Installed",     "All hooks installed"},
    {"P1: Network Ready",       "Network ready"},
    {"P1: Game Loaded",         "=== Core::OnGameLoaded() COMPLETE ==="},
    {"P1: Auto-Connecting",     "Auto-connecting to"},
    {"P1: Handshake OK",        "Handshake accepted"},
    {"P1: Entities Scanned",    "Scanning local squad characters"},
    {"P1: Remote Player Seen",  "Player '"},
    {"P1: Remote Entity Spawn", "Remote player spawned"},
    {"P1: Position Sync",       "Core::OnGameTick:"},
    {"P1: Spawn Ready",         "Spawn system ready"},
};
static constexpr int NUM_P1_MILESTONES = sizeof(g_p1Milestones) / sizeof(g_p1Milestones[0]);

// ═══════════════════════════════════════════════════════════════════════════
//  MILESTONES: Player 2 (TestClient, from stdout pipe)
// ═══════════════════════════════════════════════════════════════════════════
// ES: Hitos del Jugador 2 (TestClient), buscados en su salida estándar. "Sees P1 Entity"
//     salta con cualquier "Entity spawn:" de otra entidad, no necesariamente de P1.
// EN: Player 2 milestones (TestClient), searched in its stdout. "Sees P1 Entity" fires on
//     any other entity's "Entity spawn:", not necessarily P1's.

static Milestone g_p2Milestones[] = {
    {"P2: Connected",           "Connected to server"},
    {"P2: Handshake OK",        "Handshake OK"},
    {"P2: Host Pos Found",      "HOST POSITION FOUND"},
    {"P2: Entity Spawned",      "MY ENTITY"},
    {"P2: Sees P1 Entity",      "Entity spawn:"},
    {"P2: Recv Position",       "Position update from"},
    {"P2: Time Sync",           "TimeSync:"},
};
static constexpr int NUM_P2_MILESTONES = sizeof(g_p2Milestones) / sizeof(g_p2Milestones[0]);

// ═══════════════════════════════════════════════════════════════════════════
//  LOG MONITORING
// ═══════════════════════════════════════════════════════════════════════════

// ES: Ruta del log del Core para un proceso de Kenshi: <juego>\KenshiOnline_<PID>.log.
// EN: Core log path for a Kenshi process: <game>\KenshiOnline_<PID>.log.
static std::string FindLogFile(const std::wstring& gamePath, DWORD pid) {
    std::string logPath;
    char buf[MAX_PATH];
    int len = WideCharToMultiByte(CP_UTF8, 0, gamePath.c_str(), -1, buf, MAX_PATH, nullptr, nullptr);
    if (len > 0) logPath = std::string(buf, len - 1);
    return logPath + "\\KenshiOnline_" + std::to_string(pid) + ".log";
}

// ES: Lee solo lo nuevo del log (desde lastPos hasta el final), marca los hitos cuyo texto
//     aparezca y muestra las líneas que parecen errores graves (SEH/CRASH/FAILED + error).
//     Un hito cuyo texto quede partido entre dos lecturas podría no detectarse.
// EN: Reads only the new part of the log (from lastPos to the end), marks milestones whose
//     text appears and prints lines that look like critical errors (SEH/CRASH/FAILED + error).
//     A milestone whose text is split across two reads might be missed.
static void ScanLogForMilestones(const std::string& logPath, size_t& lastPos,
                                  Milestone* milestones, int count, const char* tag) {
    std::ifstream file(logPath, std::ios::binary);
    if (!file.is_open()) return;

    file.seekg(0, std::ios::end);
    size_t fileSize = static_cast<size_t>(file.tellg());
    if (fileSize <= lastPos) return;

    file.seekg(static_cast<std::streamoff>(lastPos));
    std::string newContent(fileSize - lastPos, '\0');
    file.read(&newContent[0], static_cast<std::streamsize>(newContent.size()));
    lastPos = fileSize;

    for (int i = 0; i < count; i++) {
        if (!milestones[i].hit && newContent.find(milestones[i].searchString) != std::string::npos) {
            milestones[i].hit = true;
            PrintOK(std::string("MILESTONE: ") + milestones[i].name);
        }
    }

    // ES: Buscar errores críticos
    // EN: Check for critical errors
    std::istringstream stream(newContent);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("SEH") != std::string::npos || line.find("CRASH") != std::string::npos ||
            line.find("FAILED") != std::string::npos) {
            if (line.find("error") != std::string::npos || line.find("ERROR") != std::string::npos) {
                PrintErr(std::string(tag) + " LOG: " + line);
            }
        }
    }
}

// ES: Lee sin bloquear de la tubería del TestClient (PeekNamedPipe para saber cuánto hay),
//     procesa las líneas completas buscando hitos y muestra las líneas interesantes.
// EN: Read non-blocking from TestClient pipe and check milestones
static void ScanPipeForMilestones(HANDLE hPipe, std::string& pipeBuffer,
                                   Milestone* milestones, int count) {
    DWORD avail = 0;
    if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
        return;

    char buf[4096];
    DWORD bytesRead = 0;
    while (avail > 0) {
        DWORD toRead = (avail < sizeof(buf)) ? avail : static_cast<DWORD>(sizeof(buf));
        if (!ReadFile(hPipe, buf, toRead, &bytesRead, nullptr) || bytesRead == 0) break;
        pipeBuffer.append(buf, bytesRead);
        avail -= bytesRead;
    }

    // ES: Procesar las líneas completas (lo que quede a medias se guarda para la siguiente vez)
    // EN: Process complete lines
    size_t pos;
    while ((pos = pipeBuffer.find('\n')) != std::string::npos) {
        std::string line = pipeBuffer.substr(0, pos);
        pipeBuffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // ES: Comprobar hitos
        // EN: Check milestones
        for (int i = 0; i < count; i++) {
            if (!milestones[i].hit && line.find(milestones[i].searchString) != std::string::npos) {
                milestones[i].hit = true;
                PrintOK(std::string("MILESTONE: ") + milestones[i].name);
            }
        }

        // ES: Mostrar las líneas interesantes de P2 (las 20 primeras y luego una de cada 50)
        // EN: Print interesting P2 lines
        if (line.find("[*]") != std::string::npos ||
            line.find("[<]") != std::string::npos ||
            line.find("ERROR") != std::string::npos) {
            // Throttle: only print first 20 lines, then every 50th
            static int p2LinesPrinted = 0;
            p2LinesPrinted++;
            if (p2LinesPrinted <= 20 || p2LinesPrinted % 50 == 0) {
                PrintP2(line);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  DASHBOARD
// ═══════════════════════════════════════════════════════════════════════════
// ES: Panel periódico (cada 15 s): hitos de P1, y en modo dual los de P2, comprobaciones
//     de autoridad/visibilidad y progreso total con color según lo conseguido.
// EN: Periodic panel (every 15 s): P1 milestones and, in dual mode, P2 milestones,
//     authority/visibility checks and total progress colored by how much passed.

static void PrintDualDashboard(DWORD kenshiPID, DWORD tcPID, int elapsedSec, bool dualMode) {
    printf("\n");
    SetColor(CYAN);
    printf("==================== LIVE TEST DASHBOARD (%ds elapsed) ====================\n", elapsedSec);
    SetColor(WHITE);

    // ES: Hitos del Jugador 1
    // EN: Player 1 milestones
    SetColor(GREEN);
    printf("  Player 1 (Kenshi PID %lu):\n", kenshiPID);
    SetColor(WHITE);
    int p1Passed = 0;
    for (int i = 0; i < NUM_P1_MILESTONES; i++) {
        if (g_p1Milestones[i].hit) {
            SetColor(GREEN);
            printf("    [PASS] %s\n", g_p1Milestones[i].name);
            p1Passed++;
        } else {
            SetColor(GRAY);
            printf("    [ -- ] %s\n", g_p1Milestones[i].name);
        }
    }

    if (dualMode) {
        printf("\n");
        SetColor(MAGENTA);
        printf("  Player 2 (TestClient PID %lu):\n", tcPID);
        SetColor(WHITE);
        int p2Passed = 0;
        for (int i = 0; i < NUM_P2_MILESTONES; i++) {
            if (g_p2Milestones[i].hit) {
                SetColor(GREEN);
                printf("    [PASS] %s\n", g_p2Milestones[i].name);
                p2Passed++;
            } else {
                SetColor(GRAY);
                printf("    [ -- ] %s\n", g_p2Milestones[i].name);
            }
        }

        // ES: Comprobación de autoridad (derivada de los hitos por índice fijo)
        // EN: Authority check
        printf("\n");
        SetColor(CYAN);
        printf("  Authority Verification:\n");
        SetColor(WHITE);

        bool p1SeesP2 = g_p1Milestones[10].hit; // "Remote Entity Spawn"
        bool p2SeesP1 = g_p2Milestones[4].hit;  // "Sees P1 Entity"
        bool p2HasEntity = g_p2Milestones[3].hit; // "Entity Spawned" (MY ENTITY)
        bool posSync = g_p2Milestones[5].hit;    // "Recv Position"

        auto printCheck = [](bool ok, const char* label) {
            if (ok) { SetColor(GREEN); printf("    [PASS] %s\n", label); }
            else    { SetColor(GRAY);  printf("    [ -- ] %s\n", label); }
        };

        printCheck(p2HasEntity, "P2 owns entity (server assigned entity ID to P2)");
        printCheck(p1SeesP2, "P1 sees P2's entity (remote spawn in Kenshi)");
        printCheck(p2SeesP1, "P2 sees P1's entity (P1 entities broadcast to P2)");
        printCheck(posSync, "Position sync (P1 positions reaching P2)");
        printCheck(p1SeesP2 && p2SeesP1, "Bidirectional entity visibility");

        printf("\n");
        int total = p1Passed + p2Passed;
        int totalMax = NUM_P1_MILESTONES + NUM_P2_MILESTONES;
        SetColor(total >= totalMax - 2 ? GREEN : (total > totalMax / 2 ? YELLOW : RED));
        printf("  Progress: P1=%d/%d  P2=%d/%d  Total=%d/%d\n",
               p1Passed, NUM_P1_MILESTONES, p2Passed, NUM_P2_MILESTONES, total, totalMax);
    } else {
        printf("\n");
        SetColor(p1Passed == NUM_P1_MILESTONES ? GREEN : YELLOW);
        printf("  Progress: %d/%d milestones\n", p1Passed, NUM_P1_MILESTONES);
    }

    SetColor(GRAY);
    printf("  Press Enter to stop and generate report...\n");
    SetColor(WHITE);
    printf("============================================================================\n\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  FINAL REPORT
// ═══════════════════════════════════════════════════════════════════════════
// ES: Informe final: PASS/FAIL de cada hito, resumen de autoridad y sincronización y
//     estado del pipeline. Devuelve 0 (éxito) si P1 alcanzó al menos 6 hitos, si no 1.
// EN: Final report: PASS/FAIL per milestone, authority and sync summary and pipeline
//     status. Returns 0 (success) if P1 hit at least 6 milestones, otherwise 1.

static int PrintFinalReport(int elapsedSec, bool dualMode) {
    printf("\n");
    SetColor(CYAN);
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                    FINAL TEST REPORT                            ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    SetColor(WHITE);

    // ES: Jugador 1
    // EN: Player 1
    printf("\n");
    SetColor(GREEN);
    printf("  Player 1 (Kenshi Game Client):\n");
    SetColor(WHITE);
    int p1Passed = 0;
    for (int i = 0; i < NUM_P1_MILESTONES; i++) {
        SetColor(g_p1Milestones[i].hit ? GREEN : RED);
        printf("    [%s] %s\n", g_p1Milestones[i].hit ? "PASS" : "FAIL", g_p1Milestones[i].name);
        if (g_p1Milestones[i].hit) p1Passed++;
    }

    int p2Passed = 0;
    if (dualMode) {
        printf("\n");
        SetColor(MAGENTA);
        printf("  Player 2 (TestClient):\n");
        SetColor(WHITE);
        for (int i = 0; i < NUM_P2_MILESTONES; i++) {
            SetColor(g_p2Milestones[i].hit ? GREEN : RED);
            printf("    [%s] %s\n", g_p2Milestones[i].hit ? "PASS" : "FAIL", g_p2Milestones[i].name);
            if (g_p2Milestones[i].hit) p2Passed++;
        }

        // ES: Resumen de autoridad
        // EN: Authority summary
        printf("\n");
        SetColor(CYAN);
        printf("  Authority & Sync Summary:\n");
        SetColor(WHITE);

        bool p1SeesP2 = g_p1Milestones[10].hit;
        bool p2SeesP1 = g_p2Milestones[4].hit;
        bool p2HasEntity = g_p2Milestones[3].hit;
        bool posSync = g_p2Milestones[5].hit;

        if (p2HasEntity)
            PrintOK("Entity ownership: P2 has server-assigned entity (authority = P2)");
        if (p1SeesP2 && p2SeesP1)
            PrintOK("Bidirectional visibility: Both players see each other's entities");
        if (posSync)
            PrintOK("Position sync: Movement data flowing between players");
        if (p2HasEntity && p1SeesP2 && p2SeesP1 && posSync)
            PrintOK("FULL MULTIPLAYER PIPELINE VERIFIED");
    }

    // ES: Resumen
    // EN: Summary
    printf("\n");
    int total = p1Passed + (dualMode ? p2Passed : 0);
    int totalMax = NUM_P1_MILESTONES + (dualMode ? NUM_P2_MILESTONES : 0);

    SetColor(total >= totalMax - 2 ? GREEN : (total > totalMax / 2 ? YELLOW : RED));
    if (dualMode)
        printf("  Result: P1=%d/%d  P2=%d/%d  Total=%d/%d in %ds\n",
               p1Passed, NUM_P1_MILESTONES, p2Passed, NUM_P2_MILESTONES, total, totalMax, elapsedSec);
    else
        printf("  Result: %d/%d milestones in %ds\n", p1Passed, NUM_P1_MILESTONES, elapsedSec);
    SetColor(WHITE);

    // ES: Estado del pipeline por umbrales de hitos alcanzados
    // EN: Pipeline status
    printf("\n");
    if (p1Passed >= 6) PrintOK("Core pipeline (DLL -> hooks -> game load -> network)");
    if (p1Passed >= 8) PrintOK("Network connection (handshake + player join)");
    if (dualMode && p2Passed >= 4) PrintOK("Second player pipeline (connect -> spawn -> sync)");
    if (dualMode && p1Passed >= 10 && p2Passed >= 5) PrintOK("Full multiplayer (spawn + sync + authority)");

    return (p1Passed >= 6) ? 0 : 1;
}

// ═══════════════════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════════════════
// ES: Punto de entrada: prepara el entorno (DLL, plugin, client.json), lanza servidor,
//     TestClient (modo dual) y Kenshi, y vigila los hitos hasta pulsar Enter o que Kenshi se cierre.
// EN: Entry point: prepares the environment (DLL, plugin, client.json), launches server,
//     TestClient (dual mode) and Kenshi, and watches milestones until Enter or Kenshi exits.

int main(int argc, char* argv[]) {
    g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    // ES: Leer argumentos (--dual/-d, --help/-h y posicionales nombre, ip, puerto)
    // EN: Parse args
    std::string playerName = "LiveTestPlayer";
    std::string serverIP = "127.0.0.1";
    int serverPort = 27800;
    bool dualMode = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--dual" || arg == "-d") {
            dualMode = true;
        } else if (arg == "--help" || arg == "-h") {
            printf("Usage:\n");
            printf("  Single:  KenshiMP.LiveTest.exe [playerName] [serverIP] [serverPort]\n");
            printf("  Dual:    KenshiMP.LiveTest.exe --dual [playerName]\n");
            printf("\n");
            printf("Options:\n");
            printf("  --dual, -d    Launch server + TestClient + Kenshi for two-player test\n");
            printf("  --help, -h    Show this help\n");
            return 0;
        } else {
            // ES: Argumentos posicionales
            // EN: Positional args
            static int posIdx = 0;
            if (posIdx == 0) playerName = arg;
            else if (posIdx == 1) serverIP = arg;
            else if (posIdx == 2) {
                try { serverPort = std::stoi(arg); } catch (...) {}
            }
            posIdx++;
        }
    }

    // ES: Nombre pseudoaleatorio del bot (Jugador 2)
    // EN: Pseudo-random bot name (Player 2)
    std::string p2Name = "TestBot_" + std::to_string(GetTickCount() % 999);

    // ES: Cabecera con la configuración del test
    // EN: Banner with the test configuration
    SetColor(CYAN);
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════════════╗\n");
    if (dualMode) {
        printf("  ║     KenshiMP Dual-Player Integration Test                   ║\n");
        printf("  ║                                                             ║\n");
        printf("  ║     P1 (Kenshi): %-40s  ║\n", playerName.c_str());
        printf("  ║     P2 (Bot):    %-40s  ║\n", p2Name.c_str());
        printf("  ║     Server:      127.0.0.1:%-5d                             ║\n", serverPort);
    } else {
        printf("  ║     KenshiMP Live Integration Test                          ║\n");
        printf("  ║     Player: %-48s  ║\n", playerName.c_str());
        printf("  ║     Server: %-38s:%d  ║\n", serverIP.c_str(), serverPort);
    }
    printf("  ╚══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    SetColor(WHITE);

    // ES: ── Paso 1: localizar Kenshi ──
    // EN: ── Step 1: Find Kenshi ──
    PrintStep("Finding Kenshi installation...");
    std::wstring gamePath = FindKenshiPath();
    if (gamePath.empty()) {
        PrintErr("Kenshi not found! Install via Steam.");
        return 1;
    }
    {
        char buf[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, gamePath.c_str(), -1, buf, MAX_PATH, nullptr, nullptr);
        PrintOK(std::string("Kenshi found at: ") + buf);
    }

    // ES: ── Paso 2: comprobar que Steam está abierto ──
    // EN: ── Step 2: Check Steam ──
    PrintStep("Checking Steam...");
    if (!IsProcessRunning(L"steam.exe") && !IsProcessRunning(L"Steam.exe")) {
        PrintErr("Steam is not running! Start Steam first.");
        return 1;
    }
    PrintOK("Steam is running");

    // ES: ── Paso 3: copiar la DLL ──
    // EN: ── Step 3: Copy DLL ──
    PrintStep("Copying KenshiMP.Core.dll...");
    if (!CopyDllToGame(gamePath)) {
        PrintErr("Could not find or copy KenshiMP.Core.dll");
        return 1;
    }

    // ES: ── Paso 4: registrar el plugin de Ogre ──
    // EN: ── Step 4: Install Ogre plugin ──
    PrintStep("Installing Ogre plugin...");
    if (!InstallOgrePlugin(gamePath)) {
        PrintErr("Failed to modify Plugins_x64.cfg");
        return 1;
    }
    PrintOK("Ogre plugin installed (Plugins_x64.cfg)");

    // ES: ── Paso 5: escribir la config del cliente (en modo dual se fuerza 127.0.0.1) ──
    // EN: ── Step 5: Write client config ──
    PrintStep("Writing client config for P1...");
    if (dualMode)
        serverIP = "127.0.0.1"; // Force local in dual mode
    if (!WriteClientConfig(playerName, serverIP, serverPort)) {
        PrintErr("Failed to write client.json");
        return 1;
    }

    // ES: ── Paso 6: arrancar el servidor ──
    //     Ojo: las dos ramas (dual y simple) son idénticas; en modo simple también se lanza
    //     un servidor local aunque se haya indicado una IP de servidor remota.
    // EN: ── Step 6: Start server ──
    //     Note: both branches (dual and single) are identical; single mode also launches a
    //     local server even when a remote server IP was given.
    HANDLE hServer = INVALID_HANDLE_VALUE;
    if (dualMode) {
        PrintStep("Starting KenshiMP server...");
        hServer = LaunchServer(gamePath);
        if (hServer == INVALID_HANDLE_VALUE) {
            PrintWarn("Server failed to start — continuing anyway (may already be running)");
        }
        PrintInfo("Waiting 2s for server startup...");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    } else {
        PrintStep("Starting KenshiMP server...");
        hServer = LaunchServer(gamePath);
        if (hServer == INVALID_HANDLE_VALUE) {
            PrintWarn("Server failed to start — continuing anyway (may already be running)");
        }
        PrintInfo("Waiting 2s for server startup...");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // ES: ── Paso 7: lanzar el TestClient (solo modo dual; si falla se degrada a modo simple) ──
    // EN: ── Step 7: Launch TestClient (dual mode only) ──
    PipedProcess testClient = {};
    if (dualMode) {
        PrintStep("Launching TestClient as Player 2...");
        testClient = LaunchTestClient(gamePath, serverIP, serverPort, p2Name);
        if (testClient.hProcess == INVALID_HANDLE_VALUE) {
            PrintErr("TestClient launch failed — dual test degraded to single mode");
            dualMode = false;
        }
    }

    // ES: ── Paso 8: lanzar Kenshi (o reutilizar una instancia ya abierta) ──
    // EN: ── Step 8: Launch Kenshi ──
    if (IsProcessRunning(L"kenshi_x64.exe")) {
        PrintWarn("Kenshi is already running! The existing instance will be used.");
        PrintInfo("If it doesn't have the mod loaded, close it and re-run this test.");
    } else {
        PrintStep("Launching Kenshi via Steam...");
        if (!LaunchKenshi()) {
            PrintErr("Failed to launch Kenshi via Steam");
            PrintInfo("Try launching Kenshi manually — the mod will auto-load via Ogre plugin");
        } else {
            PrintOK("Kenshi launch requested via Steam");
        }
    }

    // ES: ── Paso 9: esperar al proceso de Kenshi (máximo 120 s; si no, se matan servidor y bot) ──
    // EN: ── Step 9: Wait for Kenshi process ──
    PrintStep("Waiting for kenshi_x64.exe to start...");
    DWORD kenshiPID = FindKenshiPID(120);
    if (kenshiPID == 0) {
        PrintErr("Kenshi did not start within 120 seconds");
        if (testClient.hProcess != INVALID_HANDLE_VALUE) TerminateProcess(testClient.hProcess, 0);
        if (hServer != INVALID_HANDLE_VALUE) TerminateProcess(hServer, 0);
        return 1;
    }
    PrintOK("Kenshi started (PID " + std::to_string(kenshiPID) + ")");

    // ES: ── Paso 10: vigilar ambos procesos (bucle cada 2 s hasta Enter o cierre de Kenshi) ──
    // EN: ── Step 10: Monitor both processes ──
    std::string logPath = FindLogFile(gamePath, kenshiPID);
    PrintInfo("Monitoring P1 log: " + logPath);
    if (dualMode) {
        PrintInfo("Monitoring P2 stdout via pipe");
    }
    PrintInfo("Load a save or start a new game in Kenshi!");
    PrintInfo("The mod will auto-connect once the game loads.");
    printf("\n");

    size_t logPos = 0;
    std::string p2PipeBuffer;
    auto startTime = std::chrono::steady_clock::now();
    auto lastDashboard = startTime;
    bool firstLogFound = false;

    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);

    while (true) {
        // ES: Comprobar si se pulsó Enter (sin bloquear, leyendo eventos de consola)
        // EN: Check for Enter key (non-blocking)
        DWORD events = 0;
        if (GetNumberOfConsoleInputEvents(hStdin, &events) && events > 0) {
            INPUT_RECORD ir;
            DWORD read;
            while (PeekConsoleInputW(hStdin, &ir, 1, &read) && read > 0) {
                ReadConsoleInputW(hStdin, &ir, 1, &read);
                if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown &&
                    ir.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) {
                    goto done;
                }
            }
        }

        // ES: Revisar el log de P1
        // EN: Scan P1 log file
        ScanLogForMilestones(logPath, logPos, g_p1Milestones, NUM_P1_MILESTONES, "P1");

        if (!firstLogFound && logPos > 0) {
            firstLogFound = true;
            PrintP1("Log file detected — DLL is loaded!");
        }

        // ES: Revisar la tubería de P2 (modo dual)
        // EN: Scan P2 pipe (dual mode)
        if (dualMode && testClient.hReadPipe != INVALID_HANDLE_VALUE) {
            ScanPipeForMilestones(testClient.hReadPipe, p2PipeBuffer,
                                  g_p2Milestones, NUM_P2_MILESTONES);
        }

        // ES: Comprobar si Kenshi sigue abierto (si se cierra, se sale del bucle)
        // EN: Check if Kenshi is still running
        if (!IsProcessRunning(L"kenshi_x64.exe")) {
            PrintWarn("Kenshi process exited!");
            break;
        }

        // ES: Comprobar si el TestClient sigue vivo (modo dual). Ojo: si terminó, el aviso se
        //     repite en cada vuelta del bucle porque no se marca como ya notificado.
        // EN: Check if TestClient is still running (dual mode)
        //     Note: once it has exited, the warning repeats every loop iteration since it isn't
        //     flagged as already reported.
        if (dualMode && testClient.hProcess != INVALID_HANDLE_VALUE) {
            DWORD exitCode = 0;
            if (GetExitCodeProcess(testClient.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                PrintWarn("TestClient exited (code " + std::to_string(exitCode) + ")");
                // ES: Vaciar lo que quede en la tubería
                // EN: Drain remaining pipe output
                if (testClient.hReadPipe != INVALID_HANDLE_VALUE) {
                    ScanPipeForMilestones(testClient.hReadPipe, p2PipeBuffer,
                                          g_p2Milestones, NUM_P2_MILESTONES);
                }
            }
        }

        // ES: Panel cada 15 s
        // EN: Dashboard every 15s
        auto now = std::chrono::steady_clock::now();
        auto sinceDashboard = std::chrono::duration_cast<std::chrono::seconds>(now - lastDashboard);
        if (sinceDashboard.count() >= 15) {
            lastDashboard = now;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime);
            PrintDualDashboard(kenshiPID, testClient.pid, static_cast<int>(elapsed.count()), dualMode);
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

done:
    // ES: Fin de la vigilancia: informe final con el tiempo total
    // EN: End of monitoring: final report with total time
    auto endTime = std::chrono::steady_clock::now();
    auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime);
    int exitCode = PrintFinalReport(static_cast<int>(totalElapsed.count()), dualMode);

    // ES: ── Preguntar si se matan servidor y bot ──
    // EN: ── Cleanup prompt ──
    printf("\n");
    SetColor(YELLOW);
    printf("Kill server and test client? (y/n): ");
    SetColor(WHITE);

    char answer[8] = {};
    if (fgets(answer, sizeof(answer), stdin) && (answer[0] == 'y' || answer[0] == 'Y')) {
        if (dualMode && testClient.hProcess != INVALID_HANDLE_VALUE) {
            TerminateProcess(testClient.hProcess, 0);
            PrintOK("TestClient terminated");
        }
        if (hServer != INVALID_HANDLE_VALUE) {
            TerminateProcess(hServer, 0);
            PrintOK("Server terminated");
        }
    }

    // ES: Cerrar handles
    // EN: Close handles
    if (testClient.hProcess != INVALID_HANDLE_VALUE) CloseHandle(testClient.hProcess);
    if (testClient.hReadPipe != INVALID_HANDLE_VALUE) CloseHandle(testClient.hReadPipe);
    if (hServer != INVALID_HANDLE_VALUE) CloseHandle(hServer);

    printf("\nDone. P1 log: %s\n", logPath.c_str());
    return exitCode;
}
