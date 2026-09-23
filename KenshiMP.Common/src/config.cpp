// ES: Implementación de ClientConfig y ServerConfig: rutas por defecto en %APPDATA%,
//     carga/guardado en JSON (nlohmann::json) y validación de los valores cargados.
// EN: ClientConfig and ServerConfig implementation: default paths under %APPDATA%,
//     JSON load/save (nlohmann::json) and validation of loaded values.
#include "kmp/config.h"
#include "kmp/constants.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <shlobj.h>
#include <algorithm>

namespace kmp {

using json = nlohmann::json;

// ES: ── Ayudantes de validación ──
// EN: ── Validation helpers ──

// ES: Limita value al rango [lo, hi]. Usa (std::max)/(std::min) entre paréntesis para
//     esquivar las macros min/max de <windows.h>.
// EN: Clamps value to [lo, hi]. Uses parenthesized (std::max)/(std::min) to dodge
//     the min/max macros from <windows.h>.
template<typename T>
static T Clamp(T value, T lo, T hi) {
    return (std::max)(lo, (std::min)(value, hi));
}

// ES: ── ClientConfig ──
// EN: ── ClientConfig ──

// ES: Devuelve %APPDATA%\KenshiMP\client.json (crea la carpeta si no existe).
//     Si no se puede obtener APPDATA, cae a "client.json" en el directorio actual.
// EN: Returns %APPDATA%\KenshiMP\client.json (creates the folder if missing).
//     If APPDATA can't be resolved, falls back to "client.json" in the current directory.
std::string ClientConfig::GetDefaultPath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::string dir = std::string(path) + "\\KenshiMP";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\client.json";
    }
    return "client.json";
}

// ES: Devuelve %APPDATA%\KenshiMP\client_<PID>.json, un fichero por proceso de Kenshi.
// EN: Returns %APPDATA%\KenshiMP\client_<PID>.json, one file per Kenshi process.
std::string ClientConfig::GetInstancePath() {
    // ES: Ruta de config específica por PID para que varias instancias del juego no choquen.
    //     Cada proceso de Kenshi tiene su propio fichero para guardar estado.
    // EN: PID-specific config path so multiple game instances don't collide.
    //     Each Kenshi process gets its own config file for saving state.
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::string dir = std::string(path) + "\\KenshiMP";
        CreateDirectoryA(dir.c_str(), nullptr);
        DWORD pid = GetCurrentProcessId();
        return dir + "\\client_" + std::to_string(pid) + ".json";
    }
    return "client.json";
}

// ES: Lee el JSON del cliente; solo sobrescribe los campos presentes. Devuelve false si
//     el fichero no existe o el JSON es inválido (cualquier excepción se captura).
// EN: Reads the client JSON; only overwrites fields that are present. Returns false if
//     the file is missing or the JSON is invalid (any exception is caught).
bool ClientConfig::Load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    try {
        json j;
        file >> j;
        if (j.contains("playerName"))  playerName  = j["playerName"].get<std::string>();
        if (j.contains("lastServer"))  lastServer  = j["lastServer"].get<std::string>();
        if (j.contains("lastPort"))    lastPort    = j["lastPort"].get<uint16_t>();
        if (j.contains("autoConnect")) autoConnect = j["autoConnect"].get<bool>();
        if (j.contains("overlayScale")) overlayScale = j["overlayScale"].get<float>();
        if (j.contains("favoriteServers")) {
            favoriteServers = j["favoriteServers"].get<std::vector<std::string>>();
        }
        if (j.contains("masterServer")) masterServer = j["masterServer"].get<std::string>();
        if (j.contains("masterPort"))   masterPort   = j["masterPort"].get<uint16_t>();
        if (j.contains("useSyncOrchestrator")) useSyncOrchestrator = j["useSyncOrchestrator"].get<bool>();

        // ES: ── Validar los valores cargados (nombre truncado, puertos 1024-65535, escala 0,1-10) ──
        // EN: ── Validate loaded values ──
        if (playerName.size() > KMP_MAX_NAME_LENGTH)
            playerName.resize(KMP_MAX_NAME_LENGTH);
        lastPort    = Clamp<uint16_t>(lastPort, 1024, 65535);
        overlayScale = Clamp(overlayScale, 0.1f, 10.0f);
        masterPort  = Clamp<uint16_t>(masterPort, 1024, 65535);

        return true;
    } catch (...) {
        return false;
    }
}

// ES: Escribe todos los campos del cliente como JSON indentado. Devuelve false si no
//     se puede abrir el fichero.
// EN: Writes every client field as indented JSON. Returns false if the file can't be opened.
bool ClientConfig::Save(const std::string& path) const {
    json j;
    j["playerName"]  = playerName;
    j["lastServer"]  = lastServer;
    j["lastPort"]    = lastPort;
    j["autoConnect"] = autoConnect;
    j["overlayScale"] = overlayScale;
    j["favoriteServers"] = favoriteServers;
    j["masterServer"] = masterServer;
    j["masterPort"]   = masterPort;
    j["useSyncOrchestrator"] = useSyncOrchestrator;

    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << j.dump(2);
    return true;
}

// ES: ── ServerConfig ──
// EN: ── ServerConfig ──

// ES: Lee el JSON del servidor; solo sobrescribe los campos presentes y luego valida.
//     Devuelve false si el fichero no existe o el JSON es inválido.
// EN: Reads the server JSON; only overwrites fields that are present, then validates.
//     Returns false if the file is missing or the JSON is invalid.
bool ServerConfig::Load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    try {
        json j;
        file >> j;
        if (j.contains("serverName")) serverName = j["serverName"].get<std::string>();
        if (j.contains("port"))       port       = j["port"].get<uint16_t>();
        if (j.contains("maxPlayers")) maxPlayers = j["maxPlayers"].get<int>();
        if (j.contains("password"))   password   = j["password"].get<std::string>();
        if (j.contains("savePath"))   savePath   = j["savePath"].get<std::string>();
        if (j.contains("tickRate"))   tickRate   = j["tickRate"].get<int>();
        if (j.contains("pvpEnabled")) pvpEnabled = j["pvpEnabled"].get<bool>();
        if (j.contains("gameSpeed"))  gameSpeed  = j["gameSpeed"].get<float>();
        if (j.contains("masterServer")) masterServer = j["masterServer"].get<std::string>();
        if (j.contains("masterPort"))   masterPort   = j["masterPort"].get<uint16_t>();
        // ES: Nuevos campos: registro al master y modo de facciones
        // EN: New fields: master registration and faction mode
        if (j.contains("enableMasterServer")) enableMasterServer = j["enableMasterServer"].get<bool>();
        if (j.contains("factionMode")) factionMode = j["factionMode"].get<std::string>();
        if (j.contains("teamSize"))    teamSize    = j["teamSize"].get<int>();

        // ES: ── Validar los valores cargados (puertos, jugadores, tick rate 1-60, velocidad 0,1-10) ──
        // EN: ── Validate loaded values ──
        port        = Clamp<uint16_t>(port, 1024, 65535);
        maxPlayers  = Clamp(maxPlayers, 1, KMP_MAX_PLAYERS);
        tickRate    = Clamp(tickRate, 1, 60);
        gameSpeed   = Clamp(gameSpeed, 0.1f, 10.0f);
        masterPort  = Clamp<uint16_t>(masterPort, 1024, 65535);
        if (serverName.size() > KMP_MAX_NAME_LENGTH)
            serverName.resize(KMP_MAX_NAME_LENGTH);
        // ES: factionMode solo admite valores conocidos; cualquier otro cae a "single" (co-op seguro)
        // EN: factionMode only accepts known values; anything else falls back to "single" (safe co-op)
        if (factionMode != "single" && factionMode != "teams" && factionMode != "per-player")
            factionMode = "single";
        teamSize    = Clamp(teamSize, 1, KMP_MAX_PLAYERS);

        return true;
    } catch (...) {
        return false;
    }
}

// ES: Escribe todos los campos del servidor como JSON indentado (la contraseña va en
//     texto plano). Devuelve false si no se puede abrir el fichero.
// EN: Writes every server field as indented JSON (the password is stored in plain text).
//     Returns false if the file can't be opened.
bool ServerConfig::Save(const std::string& path) const {
    json j;
    j["serverName"] = serverName;
    j["port"]       = port;
    j["maxPlayers"] = maxPlayers;
    j["password"]   = password;
    j["savePath"]   = savePath;
    j["tickRate"]   = tickRate;
    j["pvpEnabled"] = pvpEnabled;
    j["gameSpeed"]  = gameSpeed;
    j["masterServer"] = masterServer;
    j["masterPort"]   = masterPort;
    // ES: Nuevos campos: registro al master deshabilitado por defecto + modo de facciones
    // EN: New fields: master registration disabled by default + faction mode
    j["enableMasterServer"] = enableMasterServer;
    j["factionMode"]  = factionMode;
    j["teamSize"]     = teamSize;

    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << j.dump(2);
    return true;
}

} // namespace kmp
