#pragma once
#include "constants.h"
#include <string>
#include <vector>
#include <cstdint>

namespace kmp {

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

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    static std::string GetDefaultPath();     // Shared path (Injector writes here)
    static std::string GetInstancePath();    // PID-specific path (Core saves here)
};

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
    // Si es false (default), el server NO se registra en el master server.
    // Para juego local/LAN no hace falta — evita el spam de reconexión al master.
    bool        enableMasterServer = false;
    // Modo de asignación de facciones:
    //   "single"     = todos los jugadores comparten la facción slot 0 (co-op puro, estable HOY)
    //   "teams"      = grupos comparten facción (slot = (id-1) / teamSize)
    //   "per-player" = cada jugador su propia facción (slot = id-1, requiere maxPlayers <= nº facciones)
    std::string factionMode  = "single";
    int         teamSize     = 1;                // Para "teams": jugadores por equipo/facción

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
};

} // namespace kmp
