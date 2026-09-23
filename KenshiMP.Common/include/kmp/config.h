// ES: Configuración persistente del cliente y del servidor (se guarda en JSON).
//     ClientConfig la usan el Injector (lanzador) y el plugin Core dentro de Kenshi;
//     ServerConfig la usa el servidor dedicado. La implementación está en src/config.cpp.
// EN: Persistent client and server configuration (stored as JSON).
//     ClientConfig is used by the Injector (launcher) and by the Core plugin inside Kenshi;
//     ServerConfig is used by the dedicated server. Implementation lives in src/config.cpp.
#pragma once
#include "constants.h"
#include <string>
#include <vector>
#include <cstdint>

namespace kmp {

// ES: Ajustes del cliente: nombre de jugador, último servidor usado, master server,
//     favoritos y opciones de la interfaz/sincronización.
// EN: Client settings: player name, last server used, master server,
//     favorites and UI/sync options.
struct ClientConfig {
    std::string playerName     = "Player";
    std::string lastServer     = "162.248.94.149";
    uint16_t    lastPort       = KMP_DEFAULT_PORT;
    bool        autoConnect    = true;
    float       overlayScale   = 1.0f;
    std::string masterServer   = "162.248.94.149";   // Master server address
    uint16_t    masterPort     = 27801;               // Master server port
    std::vector<std::string> favoriteServers = {"162.248.94.149:27800"};
    bool        useSyncOrchestrator = false; // New 7-stage sync pipeline (set true to test)

    // ES: Carga/guarda la configuración desde/en un fichero JSON. Devuelven false si falla.
    // EN: Loads/saves the configuration from/to a JSON file. Return false on failure.
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    // ES: Ruta compartida %APPDATA%\KenshiMP\client.json (la escribe el Injector) y
    //     ruta propia por proceso client_<PID>.json (la guarda el Core) para que varias
    //     instancias de Kenshi no se pisen.
    // EN: Shared path %APPDATA%\KenshiMP\client.json (written by the Injector) and
    //     per-process path client_<PID>.json (saved by Core) so several Kenshi
    //     instances don't clash.
    static std::string GetDefaultPath();     // Shared path (Injector writes here)
    static std::string GetInstancePath();    // PID-specific path (Core saves here)
};

// ES: Ajustes del servidor dedicado: nombre, puerto, límite de jugadores, contraseña,
//     partida guardada, tick rate, PvP, velocidad de juego, master server y facciones.
// EN: Dedicated server settings: name, port, player cap, password, save file,
//     tick rate, PvP, game speed, master server and factions.
struct ServerConfig {
    std::string serverName   = "KenshiMP Server";
    uint16_t    port         = KMP_DEFAULT_PORT;
    int         maxPlayers   = KMP_MAX_PLAYERS;
    std::string password;
    std::string savePath     = "world.kmpsave";
    int         tickRate     = KMP_TICK_RATE;
    bool        pvpEnabled   = true;
    float       gameSpeed    = 1.0f;
    std::string masterServer = "162.248.94.149"; // Master server address
    uint16_t    masterPort   = 27801;            // Master server port
    // ES: Si es false (default), el server NO se registra en el master server.
    //     Para juego local/LAN no hace falta — evita el spam de reconexión al master.
    // EN: If false (default), the server does NOT register with the master server.
    //     Not needed for local/LAN play — avoids master reconnect spam.
    bool        enableMasterServer = false;
    // ES: Modo de asignación de facciones:
    //       "single"     = todos los jugadores comparten la facción slot 0 (co-op puro, estable HOY)
    //       "teams"      = grupos comparten facción (slot = (id-1) / teamSize)
    //       "per-player" = cada jugador su propia facción (slot = id-1, requiere maxPlayers <= nº facciones)
    // EN: Faction assignment mode:
    //       "single"     = all players share faction slot 0 (pure co-op, stable today)
    //       "teams"      = groups share a faction (slot = (id-1) / teamSize)
    //       "per-player" = each player gets their own faction (slot = id-1, needs maxPlayers <= faction count)
    std::string factionMode  = "single";
    int         teamSize     = 1;                // Para "teams": jugadores por equipo/facción / For "teams": players per team/faction

    // ES: Carga/guarda la configuración desde/en un fichero JSON. Devuelven false si falla.
    // EN: Loads/saves the configuration from/to a JSON file. Return false on failure.
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
};

} // namespace kmp
