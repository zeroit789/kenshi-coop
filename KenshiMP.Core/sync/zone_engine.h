// ES: ZoneEngine: lógica de zonas del cliente. El mundo se divide en una rejilla de
//     zonas de KMP_ZONE_SIZE; el cliente sigue en qué zona está el jugador local, avisa
//     al servidor (C2S_ZoneRequest) cuando cambia, mantiene un índice zona -> entidades
//     (reconstruido periódicamente) y un índice jugador <-> zona de los jugadores remotos.
// EN: ZoneEngine: client zone logic. The world is split into a grid of KMP_ZONE_SIZE
//     zones; the client tracks which zone the local player is in, notifies the server
//     (C2S_ZoneRequest) when it changes, keeps a zone -> entities index (rebuilt
//     periodically) and a player <-> zone index for remote players.
#pragma once
#include "entity_registry.h"
#include "kmp/types.h"
#include "kmp/constants.h"
#include <vector>
#include <unordered_map>
#include <functional>
#include <mutex>

namespace kmp {

// ES: Evento de transición de zona (entrar/salir), de un jugador o de una entidad.
// Zone transition event
struct ZoneTransition {
    enum class Type { Enter, Leave };
    Type        type;
    ZoneCoord   zone;
    EntityID    entityId;   // INVALID_ENTITY if this is a player transition
    PlayerID    playerId;   // INVALID_PLAYER if this is an entity transition
};

// ES: Motor de zonas. Usa un mutex para los índices; la zona local se lee y escribe
//     sin mutex (se asume que solo la toca el hilo del juego).
// EN: Zone engine. Uses a mutex for the indices; the local zone is read and written
//     without the mutex (assumed to be touched only by the game thread).
class ZoneEngine {
public:
    // ES: Construye el motor sobre el registro de entidades.
    // EN: Builds the engine on top of the entity registry.
    explicit ZoneEngine(EntityRegistry& registry);

    // ES: ---- Seguimiento de la zona del jugador local ----
    // ---- Local Player Zone Tracking ----

    // ES: Se llama cada tick con la posición del jugador local.
    //     Devuelve true si su zona ha cambiado.
    // Called each tick with the local player's world position.
    // Returns true if the player's zone changed.
    bool UpdateLocalPlayerZone(const Vec3& position);

    // ES: Zona local actual.
    // EN: Current local zone.
    ZoneCoord GetLocalZone() const { return m_localZone; }

    // ES: Las zonas de interés 3x3 alrededor del jugador local.
    // Get the 3x3 interest zones around the local player
    std::vector<ZoneCoord> GetInterestZones() const;

    // ES: ---- Índices de zona en caché (se reconstruyen periódicamente) ----
    // ---- Cached Zone Indices (rebuilt periodically) ----

    // ES: Reconstruye la caché de zonas desde el registro de entidades.
    // Rebuild zone caches from the entity registry.
    void RebuildZoneIndex();

    // ES: Entidades de una zona (consulta O(1) tras reconstruir). Devuelve una referencia
    //     a la caché: deja de ser válida si se vuelve a llamar a RebuildZoneIndex/Reset.
    // Get entity IDs in a specific zone (O(1) lookup after rebuild)
    const std::vector<EntityID>& GetEntitiesInZone(const ZoneCoord& zone) const;

    // ES: Todas las zonas con entidades.
    // Get all populated zones
    std::vector<ZoneCoord> GetPopulatedZones() const;

    // ES: Número de entidades en una zona.
    // Get zone population count
    size_t GetZonePopulation(const ZoneCoord& zone) const;

    // ES: ---- Relación jugador-zona ----
    // ---- Player-Zone Binding ----

    // ES: Actualiza la zona de un jugador remoto (a partir de sus posiciones).
    // Update a remote player's zone (from their position updates)
    void UpdatePlayerZone(PlayerID playerId, const ZoneCoord& zone);

    // ES: Todos los jugadores de una zona.
    // Get all players in a specific zone
    std::vector<PlayerID> GetPlayersInZone(const ZoneCoord& zone) const;

    // ES: En qué zona está un jugador ((0,0) si no se conoce).
    // Get which zone a player is in
    ZoneCoord GetPlayerZone(PlayerID playerId) const;

    // ES: Deja de seguir a un jugador (al desconectar).
    // Remove player from zone tracking (on disconnect)
    void RemovePlayer(PlayerID playerId);

    // ES: ---- Consultas de interés ----
    // ---- Interest Queries ----

    // ES: ¿Está esta zona dentro del rango de interés del jugador local?
    // Is this zone in the local player's interest range?
    bool IsInRange(const ZoneCoord& entityZone) const;

    // ES: ¿Debe sincronizarse esta entidad? (comprueba adyacencia de su zona)
    // Should this entity be synced (convenience: checks zone adjacency)
    bool ShouldSync(EntityID entityId) const;

    // ES: ---- Eventos + red ---- Callback de transiciones de zona y función de envío de
    //     paquetes (se inyecta para no depender de Core).
    // ---- Events + Networking ----

    using TransitionCallback = std::function<void(const ZoneTransition&)>;
    void SetTransitionCallback(TransitionCallback cb) { m_transitionCb = std::move(cb); }

    using SendFn = std::function<void(const uint8_t* data, size_t size, int channel, bool reliable)>;
    void SetSendCallback(SendFn fn) { m_sendFn = std::move(fn); }

    // ES: ---- Reinicio ----
    // ---- Reset ----
    void Reset();

private:
    // ES: Envía al servidor la nueva zona local.
    // EN: Sends the new local zone to the server.
    void NotifyZoneChange();

    // ES: Empaqueta (x, y) de la zona en una clave de 64 bits (x en los 32 bits altos).
    // EN: Packs the zone (x, y) into a 64-bit key (x in the high 32 bits).
    static uint64_t ZoneKey(const ZoneCoord& z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(z.x)) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(z.y));
    }

    // ES: Registro, zona local actual y anterior.
    // EN: Registry, current and previous local zone.
    EntityRegistry& m_registry;
    ZoneCoord m_localZone;
    ZoneCoord m_prevLocalZone;

    // ES: Caché zona -> lista de entidades (la reconstruye RebuildZoneIndex).
    // Zone -> entity list cache (rebuilt by RebuildZoneIndex)
    std::unordered_map<uint64_t, std::vector<EntityID>> m_zoneEntities;
    static const std::vector<EntityID> s_emptyList;

    // ES: Jugador -> zona.
    // Player -> zone mapping
    std::unordered_map<PlayerID, ZoneCoord> m_playerZones;
    // ES: Zona -> jugadores (índice inverso).
    // Zone -> player set (inverse index)
    std::unordered_map<uint64_t, std::vector<PlayerID>> m_zonePlayers;

    // ES: Callbacks inyectados y mutex de los índices.
    // EN: Injected callbacks and the index mutex.
    TransitionCallback m_transitionCb;
    SendFn m_sendFn;
    mutable std::mutex m_mutex;
};

} // namespace kmp
