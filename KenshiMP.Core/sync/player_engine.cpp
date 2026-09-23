// ES: Implementación de PlayerEngine (ver player_engine.h).
// EN: PlayerEngine implementation (see player_engine.h).
#include "player_engine.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>

namespace kmp {

// ES: Constructor: guarda la referencia al controlador.
// EN: Constructor: stores the controller reference.
PlayerEngine::PlayerEngine(PlayerController& controller)
    : m_controller(controller) {}

// ES: ---- Máquina de estados ----
// ---- State Machine ----

// ES: Cambia el estado del jugador local o de una sesión remota existente.
// EN: Changes the state of the local player or of an existing remote session.
void PlayerEngine::SetState(PlayerID id, PlayerState newState) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (id == m_localPlayerId) {
        m_localState = newState;
        spdlog::info("PlayerEngine: Local player state -> {}",
                     static_cast<int>(newState));
        return;
    }

    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) {
        it->second.state = newState;
        spdlog::info("PlayerEngine: Player {} ({}) state -> {}",
                     id, it->second.name, static_cast<int>(newState));
    }
}

// ES: Estado de un jugador; Disconnected si no se conoce.
// EN: A player's state; Disconnected if unknown.
PlayerState PlayerEngine::GetState(PlayerID id) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (id == m_localPlayerId) return m_localState;

    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) {
        return it->second.state;
    }
    return PlayerState::Disconnected;
}

// ES: ---- Registro ----
// ---- Registration ----

// ES: El servidor aceptó el handshake: guarda id y nombre locales y pasa a Loading.
// EN: The server accepted the handshake: stores local id and name and moves to Loading.
void PlayerEngine::OnHandshakeAck(PlayerID localId, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_localPlayerId = localId;
    m_localPlayerName = name;
    m_localState = PlayerState::Loading;

    spdlog::info("PlayerEngine: Local player initialized id={} name='{}'", localId, name);
}

// ES: Crea (o reemplaza) la sesión de un jugador remoto, directamente en InGame.
// EN: Creates (or replaces) a remote player's session, directly in InGame.
void PlayerEngine::OnRemotePlayerJoined(PlayerID id, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::steady_clock::now();
    PlayerSession session;
    session.playerId = id;
    session.name = name;
    session.state = PlayerState::InGame;
    session.joinTime = now;
    session.lastActivity = now;
    session.lastPositionUpdate = now;
    m_sessions[id] = std::move(session);

    spdlog::info("PlayerEngine: Remote player joined id={} name='{}'", id, name);
}

// ES: Marca al jugador remoto como Disconnected. Nota: la sesión NO se borra del mapa.
// EN: Marks the remote player as Disconnected. Note: the session is NOT removed from the map.
void PlayerEngine::OnRemotePlayerLeft(PlayerID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) {
        it->second.state = PlayerState::Disconnected;
        spdlog::info("PlayerEngine: Remote player left id={} name='{}'", id, it->second.name);
    }
}

// ES: Snapshot del mundo recibido: el jugador local pasa a InGame.
// EN: World snapshot received: the local player moves to InGame.
void PlayerEngine::OnWorldSnapshotReceived(int entityCount) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_localState = PlayerState::InGame;
    spdlog::info("PlayerEngine: World snapshot received ({} entities), state -> InGame", entityCount);
}

// ES: ---- Seguimiento de actividad ----
// ---- Activity Tracking ----

// ES: Actualiza la última actividad de un jugador remoto.
// EN: Updates a remote player's last activity.
void PlayerEngine::RecordActivity(PlayerID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) {
        it->second.lastActivity = std::chrono::steady_clock::now();

        // ES: Si estaba AFK, vuelve a InGame.
        // If player was AFK, transition back to InGame
        if (it->second.state == PlayerState::AFK) {
            it->second.state = PlayerState::InGame;
            spdlog::info("PlayerEngine: Player {} ({}) is no longer AFK",
                         id, it->second.name);
        }
    }
}

// ES: Guarda posición y zona de un jugador remoto (también cuenta como actividad).
// EN: Stores a remote player's position and zone (also counts as activity).
void PlayerEngine::UpdatePlayerPosition(PlayerID id, const Vec3& pos, const ZoneCoord& zone) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) {
        it->second.position = pos;
        it->second.zone = zone;
        it->second.lastPositionUpdate = std::chrono::steady_clock::now();
        it->second.lastActivity = it->second.lastPositionUpdate;
    }
}

// ES: Pasa a AFK a los jugadores InGame sin actividad durante afkTimeoutSeconds y
//     devuelve los que acaban de pasar a AFK.
// EN: Moves InGame players with no activity for afkTimeoutSeconds to AFK and
//     returns the ones that just became AFK.
std::vector<PlayerID> PlayerEngine::CheckAFK(float afkTimeoutSeconds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PlayerID> newlyAfk;
    auto now = std::chrono::steady_clock::now();

    for (auto& [id, session] : m_sessions) {
        if (session.state != PlayerState::InGame) continue;

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - session.lastActivity).count();
        if (elapsed >= static_cast<long long>(afkTimeoutSeconds)) {
            session.state = PlayerState::AFK;
            newlyAfk.push_back(id);
            spdlog::info("PlayerEngine: Player {} ({}) is now AFK ({}s idle)",
                         id, session.name, elapsed);
        }
    }
    return newlyAfk;
}

// ES: ---- Consultas ----
// ---- Query API ----

// ES: Puntero a la sesión (nullptr si no existe). Ojo: el puntero se usa fuera del
//     mutex y puede quedar inválido si el mapa cambia.
// EN: Pointer to the session (nullptr if missing). Note: the pointer is used outside
//     the mutex and may become invalid if the map changes.
const PlayerSession* PlayerEngine::GetSession(PlayerID id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) {
        return &it->second;
    }
    return nullptr;
}

// ES: Jugadores remotos en un estado dado.
// EN: Remote players in a given state.
std::vector<PlayerID> PlayerEngine::GetByState(PlayerState state) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PlayerID> result;
    for (const auto& [id, session] : m_sessions) {
        if (session.state == state) {
            result.push_back(id);
        }
    }
    return result;
}

// ES: Jugadores remotos en una zona dada.
// EN: Remote players in a given zone.
std::vector<PlayerID> PlayerEngine::GetByZone(const ZoneCoord& zone) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PlayerID> result;
    for (const auto& [id, session] : m_sessions) {
        if (session.zone == zone) {
            result.push_back(id);
        }
    }
    return result;
}

// ES: Primer jugador cuyo nombre contiene partialName (sin distinguir mayúsculas).
// EN: First player whose name contains partialName (case-insensitive).
PlayerID PlayerEngine::FindByName(const std::string& partialName) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    // ES: Coincidencia parcial sin distinguir mayúsculas/minúsculas.
    // Case-insensitive partial match
    std::string lowerPartial = partialName;
    std::transform(lowerPartial.begin(), lowerPartial.end(), lowerPartial.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& [id, session] : m_sessions) {
        std::string lowerName = session.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerName.find(lowerPartial) != std::string::npos) {
            return id;
        }
    }
    return INVALID_PLAYER;
}

// ES: Copia de todas las sesiones remotas.
// EN: Copy of all remote sessions.
std::vector<PlayerSession> PlayerEngine::GetAllSessions() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PlayerSession> result;
    result.reserve(m_sessions.size());
    for (const auto& [id, session] : m_sessions) {
        result.push_back(session);
    }
    return result;
}

// ES: Cuenta las sesiones remotas por estado.
// EN: Counts remote sessions by state.
PlayerEngine::StateCount PlayerEngine::GetStateCounts() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    StateCount counts;
    for (const auto& [id, session] : m_sessions) {
        switch (session.state) {
        case PlayerState::Connecting:   counts.connecting++; break;
        case PlayerState::Loading:      counts.loading++; break;
        case PlayerState::InGame:       counts.inGame++; break;
        case PlayerState::AFK:          counts.afk++; break;
        case PlayerState::Disconnected: counts.disconnected++; break;
        }
    }
    return counts;
}

// ES: ---- Jugador local ---- (lecturas sin mutex)
// ---- Local Player ----

PlayerID PlayerEngine::GetLocalPlayerId() const {
    return m_localPlayerId;
}

const std::string& PlayerEngine::GetLocalPlayerName() const {
    return m_localPlayerName;
}

// ES: ---- Reinicio ----
// ---- Reset ----

// ES: Borra sesiones y datos del jugador local y vuelve al estado inicial.
// EN: Clears sessions and local player data and goes back to the initial state.
void PlayerEngine::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessions.clear();
    m_localPlayerId = INVALID_PLAYER;
    m_localPlayerName.clear();
    m_localState = PlayerState::Connecting;
    m_localZone = ZoneCoord{0, 0};
    spdlog::info("PlayerEngine: Reset");
}

} // namespace kmp
