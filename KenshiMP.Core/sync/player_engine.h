// ES: PlayerEngine: seguimiento de las sesiones de jugadores en el cliente. Lleva una
//     máquina de estados por jugador (conectando, cargando, en juego, AFK,
//     desconectado), su zona/posición, actividad (para detectar AFK) y consultas
//     por estado, zona o nombre. También guarda el id/nombre/estado del jugador local.
// EN: PlayerEngine: client-side tracking of player sessions. Keeps a per-player
//     state machine (connecting, loading, in game, AFK, disconnected), their
//     zone/position, activity (for AFK detection) and queries by state, zone or name.
//     It also stores the local player's id/name/state.
#pragma once
#include "../game/player_controller.h"
#include "kmp/types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <mutex>

namespace kmp {

// ES: Estados del ciclo de vida de un jugador.
// Player lifecycle states
enum class PlayerState : uint8_t {
    Connecting   = 0,  // Handshake in progress
    Loading      = 1,  // World snapshot being received
    InGame       = 2,  // Fully loaded, syncing
    AFK          = 3,  // No input for extended period
    Disconnected = 4,  // Cleaned up, will be removed
};

// ES: Información ampliada de la sesión de un jugador: id, nombre, estado, zona,
//     posición, ping, marcas de tiempo (entrada, última actividad, última posición),
//     número de entidades y si es el host.
// Extended player session info
struct PlayerSession {
    PlayerID    playerId    = INVALID_PLAYER;
    std::string name;
    PlayerState state       = PlayerState::Connecting;
    ZoneCoord   zone;
    Vec3        position;
    uint32_t    ping        = 0;

    // Session timing
    std::chrono::steady_clock::time_point joinTime;
    std::chrono::steady_clock::time_point lastActivity;
    std::chrono::steady_clock::time_point lastPositionUpdate;

    // Entity summary
    int entityCount = 0;
    bool isHost     = false;
};

// ES: Motor de jugadores. Thread-safe con un mutex. Guarda una referencia a
//     PlayerController, pero en el .cpp actual no se le delega nada (solo se expone
//     con Controller()).
// EN: Player engine. Thread-safe with one mutex. Keeps a reference to
//     PlayerController, but the current .cpp does not delegate anything to it (it is
//     only exposed through Controller()).
class PlayerEngine {
public:
    // ES: Construye el motor sobre el controlador de jugadores del juego.
    // EN: Builds the engine on top of the game's player controller.
    explicit PlayerEngine(PlayerController& controller);

    // ES: ---- Máquina de estados ---- (cambiar / consultar el estado de un jugador;
    //     si el id es el local, se usa el estado local).
    // ---- State Machine ----

    void SetState(PlayerID id, PlayerState newState);
    PlayerState GetState(PlayerID id) const;

    // ES: ---- Registro ---- Eventos de red: handshake aceptado (jugador local), jugador
    //     remoto entra/sale, snapshot del mundo recibido.
    // ---- Registration (delegates to PlayerController + adds session tracking) ----

    void OnHandshakeAck(PlayerID localId, const std::string& name);
    void OnRemotePlayerJoined(PlayerID id, const std::string& name);
    void OnRemotePlayerLeft(PlayerID id);
    void OnWorldSnapshotReceived(int entityCount);

    // ES: ---- Seguimiento de actividad ---- Registrar actividad, actualizar posición/zona
    //     y detectar jugadores AFK (por defecto 300 s sin actividad).
    // ---- Activity Tracking ----

    void RecordActivity(PlayerID id);
    void UpdatePlayerPosition(PlayerID id, const Vec3& pos, const ZoneCoord& zone);
    std::vector<PlayerID> CheckAFK(float afkTimeoutSeconds = 300.f);

    // ES: ---- Consultas ---- Sesión por id, jugadores por estado o zona, búsqueda por
    //     nombre parcial, copia de todas las sesiones.
    // ---- Query API ----

    const PlayerSession* GetSession(PlayerID id) const;
    std::vector<PlayerID> GetByState(PlayerState state) const;
    std::vector<PlayerID> GetByZone(const ZoneCoord& zone) const;
    PlayerID FindByName(const std::string& partialName) const;
    std::vector<PlayerSession> GetAllSessions() const;

    // ES: Número de jugadores remotos en cada estado.
    // EN: Number of remote players in each state.
    struct StateCount {
        int connecting = 0, loading = 0, inGame = 0, afk = 0, disconnected = 0;
    };
    StateCount GetStateCounts() const;

    // ES: ---- Jugador local ---- id, nombre, estado y zona del jugador local.
    // ---- Local Player ----

    PlayerID GetLocalPlayerId() const;
    const std::string& GetLocalPlayerName() const;
    PlayerState GetLocalState() const { return m_localState; }
    ZoneCoord GetLocalZone() const { return m_localZone; }
    void SetLocalZone(const ZoneCoord& zone) { m_localZone = zone; }

    // ES: ---- Acceso directo al controlador ----
    // ---- Passthrough ----

    PlayerController& Controller() { return m_controller; }
    const PlayerController& Controller() const { return m_controller; }

    // ES: ---- Reinicio ---- Olvida todas las sesiones y el jugador local (al desconectar).
    // ---- Reset ----
    void Reset();

private:
    // ES: Datos internos: controlador, mutex, estado/zona/id/nombre del jugador local y
    //     mapa de sesiones de jugadores remotos.
    // EN: Internal data: controller, mutex, local player state/zone/id/name and map of
    //     remote player sessions.
    PlayerController& m_controller;
    mutable std::mutex m_mutex;

    PlayerState m_localState = PlayerState::Connecting;
    ZoneCoord   m_localZone;
    PlayerID    m_localPlayerId = INVALID_PLAYER;
    std::string m_localPlayerName;

    std::unordered_map<PlayerID, PlayerSession> m_sessions;
};

} // namespace kmp
