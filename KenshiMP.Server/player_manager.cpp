// ES: player_manager.cpp - Implementación de PlayerManager (nombres, baneos, AFK, rate limit).
// EN: player_manager.cpp - PlayerManager implementation (names, bans, AFK, rate limit).
#include "player_manager.h"
#include "server.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace kmp {

// ES: ── Gestión de nombres ──
// EN: ── Name management ──

// ES: Copia del texto en minúsculas (ASCII) para comparar sin distinguir mayúsculas.
// EN: Lowercase (ASCII) copy of the text for case-insensitive comparisons.
static std::string ToLower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// ES: Tres pasadas de menor a mayor laxitud: exacto, prefijo y subcadena.
// EN: Three passes from strictest to loosest: exact, prefix and substring.
PlayerID PlayerManager::FindByName(
    const std::unordered_map<PlayerID, ConnectedPlayer>& players,
    const std::string& name) {

    std::string needle = ToLower(name);

    // ES: Primero coincidencia exacta.
    // EN: Exact match first
    for (auto& [id, player] : players) {
        if (ToLower(player.name) == needle) return id;
    }

    // ES: Coincidencia parcial (prefijo).
    // EN: Partial match (prefix)
    for (auto& [id, player] : players) {
        std::string lower = ToLower(player.name);
        if (lower.find(needle) == 0) return id;
    }

    // ES: Coincidencia por subcadena.
    // EN: Substring match
    for (auto& [id, player] : players) {
        std::string lower = ToLower(player.name);
        if (lower.find(needle) != std::string::npos) return id;
    }

    return 0;
}

// ES: Solo coincidencia exacta sin distinguir mayúsculas.
// EN: Exact case-insensitive match only.
PlayerID PlayerManager::FindByExactName(
    const std::unordered_map<PlayerID, ConnectedPlayer>& players,
    const std::string& name) {

    std::string needle = ToLower(name);
    for (auto& [id, player] : players) {
        if (ToLower(player.name) == needle) return id;
    }
    return 0;
}

// ES: Nombre ocupado = existe un jugador con ese nombre exacto.
// EN: Name taken = a player with that exact name exists.
bool PlayerManager::IsNameTaken(
    const std::unordered_map<PlayerID, ConnectedPlayer>& players,
    const std::string& name) {

    return FindByExactName(players, name) != 0;
}

// ES: Prueba base, base_2 ... base_99; si todos están ocupados añade un número aleatorio.
// EN: Tries base, base_2 ... base_99; if all are taken appends a random number.
std::string PlayerManager::MakeUniqueName(
    const std::unordered_map<PlayerID, ConnectedPlayer>& players,
    const std::string& baseName) {

    if (!IsNameTaken(players, baseName)) return baseName;

    for (int suffix = 2; suffix < 100; suffix++) {
        std::string candidate = baseName + "_" + std::to_string(suffix);
        if (!IsNameTaken(players, candidate)) return candidate;
    }
    return baseName + "_" + std::to_string(rand() % 9999);
}

// ES: ── Lista de baneos ──
// EN: ── Ban list ──

// ES: Añade/quita una IP de la lista de baneos, consulta si está baneada y devuelve la lista.
// EN: Adds/removes an IP from the ban list, checks if it is banned and returns the list.
void PlayerManager::BanIP(const std::string& ip) {
    m_bannedIPs.insert(ip);
    spdlog::info("PlayerManager: Banned IP {}", ip);
}

void PlayerManager::UnbanIP(const std::string& ip) {
    m_bannedIPs.erase(ip);
    spdlog::info("PlayerManager: Unbanned IP {}", ip);
}

bool PlayerManager::IsIPBanned(const std::string& ip) const {
    return m_bannedIPs.count(ip) > 0;
}

std::vector<std::string> PlayerManager::GetBanList() const {
    return std::vector<std::string>(m_bannedIPs.begin(), m_bannedIPs.end());
}

// ES: ── Detección de AFK ──
// EN: ── AFK detection ──

// ES: Devuelve los jugadores cuya última actualización es más antigua que timeoutSeconds.
// EN: Returns players whose last update is older than timeoutSeconds.
std::vector<PlayerID> PlayerManager::GetAFKPlayers(
    const std::unordered_map<PlayerID, ConnectedPlayer>& players,
    float currentTime, float timeoutSeconds) {

    std::vector<PlayerID> afk;
    for (auto& [id, player] : players) {
        if (currentTime - player.lastUpdate > timeoutSeconds) {
            afk.push_back(id);
        }
    }
    return afk;
}

// ES: ── Limitación de mensajes ──
// EN: ── Rate limiting ──

// ES: Cuenta los mensajes del jugador dentro de la ventana; true si llega a maxMessages.
//     Un jugador sin registros nunca está limitado.
// EN: Counts the player's messages within the window; true if it reaches maxMessages.
//     A player with no records is never throttled.
bool PlayerManager::CheckRateLimit(PlayerID id, float currentTime,
                                    float windowSeconds, int maxMessages) {
    auto it = m_rateLimits.find(id);
    if (it == m_rateLimits.end()) return false;

    auto& timestamps = it->second.timestamps;

    // ES: Cuenta mensajes dentro de la ventana de tiempo.
    // EN: Count messages within the window
    int count = 0;
    for (auto& t : timestamps) {
        if (currentTime - t <= windowSeconds) count++;
    }
    return count >= maxMessages;
}

// ES: Apunta la marca de tiempo de un mensaje del jugador.
// EN: Records the timestamp of one of the player's messages.
void PlayerManager::RecordMessage(PlayerID id, float currentTime) {
    m_rateLimits[id].timestamps.push_back(currentTime);
}

// ES: Borra marcas de tiempo más viejas que windowSeconds para que los vectores no crezcan sin fin.
// EN: Drops timestamps older than windowSeconds so the vectors do not grow forever.
void PlayerManager::CleanupRateLimits(float currentTime, float windowSeconds) {
    for (auto& [id, entry] : m_rateLimits) {
        entry.timestamps.erase(
            std::remove_if(entry.timestamps.begin(), entry.timestamps.end(),
                           [&](float t) { return currentTime - t > windowSeconds; }),
            entry.timestamps.end());
    }
}

} // namespace kmp
