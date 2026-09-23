// ES: player_manager.h - PlayerManager: utilidades de jugadores (búsqueda por nombre, nombres
//     únicos, lista de IPs baneadas, detección de AFK y limitación de mensajes por segundo).
//     Se compila en el servidor pero GameServer no lo usa a fecha de este comentario.
// EN: player_manager.h - PlayerManager: player helpers (name lookup, unique names, banned IP
//     list, AFK detection and per-second message rate limiting).
//     Compiled into the server but GameServer does not use it as of this comment.
#pragma once
#include "kmp/types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>

namespace kmp {

// ES: Declaración adelantada; la definición real está en server.h.
// EN: Forward declaration; the real definition lives in server.h.
struct ConnectedPlayer; // Forward declaration from server.h

// ES: Utilidades de gestión de jugadores: búsqueda por nombre, baneos, AFK y rate limit.
//     Las funciones de nombre y AFK son estáticas; baneos y rate limit guardan estado.
// EN: Player management utilities for the dedicated server.
// Provides name lookup, ban list, AFK detection, and rate limiting.
class PlayerManager {
public:
    // ES: ── Gestión de nombres ──
    // EN: ── Name management ──

    // ES: Busca un jugador por nombre sin distinguir mayúsculas: primero exacto, luego prefijo,
    //     luego subcadena. Devuelve el ID o 0 si no lo encuentra.
    // EN: Find a player by name (case-insensitive partial match).
    // Returns the player ID, or 0 if not found.
    static PlayerID FindByName(const std::unordered_map<PlayerID, ConnectedPlayer>& players,
                                const std::string& name);

    // ES: Busca por nombre exacto sin distinguir mayúsculas; 0 si no existe.
    // EN: Find a player by exact name (case-insensitive).
    static PlayerID FindByExactName(const std::unordered_map<PlayerID, ConnectedPlayer>& players,
                                     const std::string& name);

    // ES: Indica si el nombre ya está en uso (sin distinguir mayúsculas).
    // EN: Check if a name is already taken (case-insensitive).
    static bool IsNameTaken(const std::unordered_map<PlayerID, ConnectedPlayer>& players,
                            const std::string& name);

    // ES: Genera un nombre único si el pedido está ocupado (añade _2, _3, ...).
    // EN: Generate a unique name if the requested one is taken (appends _2, _3, etc.)
    static std::string MakeUniqueName(const std::unordered_map<PlayerID, ConnectedPlayer>& players,
                                       const std::string& baseName);

    // ES: ── Lista de baneos por IP (solo en memoria, no se guarda en disco) ──
    // EN: ── IP ban list (in memory only, not persisted) ──
    // ── Ban list ──

    void BanIP(const std::string& ip);
    void UnbanIP(const std::string& ip);
    bool IsIPBanned(const std::string& ip) const;
    std::vector<std::string> GetBanList() const;

    // ES: ── Detección de AFK ──
    // EN: ── AFK detection ──

    // ES: Jugadores que no han enviado actualizaciones en timeoutSeconds (usa ConnectedPlayer::lastUpdate).
    // EN: Get players who haven't sent updates in `timeoutSeconds`.
    static std::vector<PlayerID> GetAFKPlayers(
        const std::unordered_map<PlayerID, ConnectedPlayer>& players,
        float currentTime, float timeoutSeconds = 300.f);

    // ES: ── Limitación de mensajes ──
    // EN: ── Rate limiting ──

    // ES: CheckRateLimit: true si el jugador superó maxMessages en la ventana (hay que frenarlo).
    //     RecordMessage apunta un mensaje; CleanupRateLimits borra marcas de tiempo viejas.
    // EN: Check if a player has exceeded message rate (returns true if should be throttled).
    bool CheckRateLimit(PlayerID id, float currentTime, float windowSeconds = 1.f, int maxMessages = 10);
    void RecordMessage(PlayerID id, float currentTime);
    void CleanupRateLimits(float currentTime, float windowSeconds = 5.f);

private:
    // ES: IPs baneadas y, por jugador, marcas de tiempo de sus mensajes recientes.
    // EN: Banned IPs and, per player, timestamps of their recent messages.
    std::unordered_set<std::string> m_bannedIPs;

    struct RateLimitEntry {
        std::vector<float> timestamps;
    };
    std::unordered_map<PlayerID, RateLimitEntry> m_rateLimits;
};

} // namespace kmp
