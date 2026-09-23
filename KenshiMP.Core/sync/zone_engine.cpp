// ES: Implementación de ZoneEngine (ver zone_engine.h).
// EN: ZoneEngine implementation (see zone_engine.h).
#include "zone_engine.h"
#include "kmp/protocol.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace kmp {

// ES: Lista vacía que se devuelve cuando una zona no tiene entidades en caché.
// EN: Empty list returned when a zone has no cached entities.
const std::vector<EntityID> ZoneEngine::s_emptyList;

// ES: Constructor: guarda la referencia al registro.
// EN: Constructor: stores the registry reference.
ZoneEngine::ZoneEngine(EntityRegistry& registry)
    : m_registry(registry) {}

// ES: ---- Seguimiento de la zona del jugador local ----
// ---- Local Player Zone Tracking ----

// ES: Calcula la zona de la posición; si cambia, dispara los eventos Leave/Enter y
//     avisa al servidor.
// EN: Computes the zone of the position; if it changed, fires Leave/Enter events and
//     notifies the server.
bool ZoneEngine::UpdateLocalPlayerZone(const Vec3& position) {
    ZoneCoord newZone = ZoneCoord::FromWorldPos(position, KMP_ZONE_SIZE);
    if (newZone == m_localZone) return false;

    m_prevLocalZone = m_localZone;
    m_localZone = newZone;

    spdlog::debug("ZoneEngine: Player moved to zone ({}, {})", newZone.x, newZone.y);

    // ES: Disparar el callback de transición (salida de la zona vieja, entrada en la nueva).
    // Fire transition callback
    if (m_transitionCb) {
        ZoneTransition leave;
        leave.type = ZoneTransition::Type::Leave;
        leave.zone = m_prevLocalZone;
        leave.entityId = INVALID_ENTITY;
        leave.playerId = INVALID_PLAYER; // local player
        m_transitionCb(leave);

        ZoneTransition enter;
        enter.type = ZoneTransition::Type::Enter;
        enter.zone = m_localZone;
        enter.entityId = INVALID_ENTITY;
        enter.playerId = INVALID_PLAYER;
        m_transitionCb(enter);
    }

    // ES: Avisar al servidor del cambio de zona.
    // Notify server of zone change
    NotifyZoneChange();

    return true;
}

// ES: Zonas en [-KMP_INTEREST_RADIUS, +KMP_INTEREST_RADIUS] alrededor de la zona local.
// EN: Zones within [-KMP_INTEREST_RADIUS, +KMP_INTEREST_RADIUS] around the local zone.
std::vector<ZoneCoord> ZoneEngine::GetInterestZones() const {
    std::vector<ZoneCoord> zones;
    zones.reserve(9);
    for (int dx = -KMP_INTEREST_RADIUS; dx <= KMP_INTEREST_RADIUS; dx++) {
        for (int dy = -KMP_INTEREST_RADIUS; dy <= KMP_INTEREST_RADIUS; dy++) {
            zones.emplace_back(m_localZone.x + dx, m_localZone.y + dy);
        }
    }
    return zones;
}

// ES: ---- Índices de zona en caché ----
// ---- Cached Zone Indices ----

// ES: Vacía la caché y la rellena con las entidades de las zonas de interés.
// EN: Clears the cache and fills it with the entities of the interest zones.
void ZoneEngine::RebuildZoneIndex() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_zoneEntities.clear();

    // ES: Reunir las entidades de las zonas de interés. Ojo: el comentario original dice
    //     que también se recorren "las zonas que tienen entidades", pero el código solo
    //     recorre las zonas de interés.
    // Gather entities from all interest zones
    // We scan the interest zones and any zones that have entities
    auto interestZones = GetInterestZones();
    for (const auto& zone : interestZones) {
        auto entities = m_registry.GetEntitiesInZone(zone);
        if (!entities.empty()) {
            m_zoneEntities[ZoneKey(zone)] = std::move(entities);
        }
    }
}

// ES: Referencia a la lista en caché de la zona (o a la lista vacía).
// EN: Reference to the zone's cached list (or to the empty list).
const std::vector<EntityID>& ZoneEngine::GetEntitiesInZone(const ZoneCoord& zone) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_zoneEntities.find(ZoneKey(zone));
    if (it != m_zoneEntities.end()) {
        return it->second;
    }
    return s_emptyList;
}

// ES: Decodifica las claves de la caché para devolver las zonas con entidades.
// EN: Decodes the cache keys to return the zones that have entities.
std::vector<ZoneCoord> ZoneEngine::GetPopulatedZones() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ZoneCoord> result;
    for (const auto& [key, entities] : m_zoneEntities) {
        if (!entities.empty()) {
            int32_t x = static_cast<int32_t>(key >> 32);
            int32_t y = static_cast<int32_t>(key & 0xFFFFFFFF);
            result.emplace_back(x, y);
        }
    }
    return result;
}

// ES: Tamaño de la lista en caché de la zona.
// EN: Size of the zone's cached list.
size_t ZoneEngine::GetZonePopulation(const ZoneCoord& zone) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_zoneEntities.find(ZoneKey(zone));
    if (it != m_zoneEntities.end()) {
        return it->second.size();
    }
    return 0;
}

// ES: ---- Relación jugador-zona ----
// ---- Player-Zone Binding ----

// ES: Mueve al jugador de su zona anterior a la nueva en ambos índices.
// EN: Moves the player from their previous zone to the new one in both indices.
void ZoneEngine::UpdatePlayerZone(PlayerID playerId, const ZoneCoord& zone) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // ES: Quitarlo de la lista de jugadores de su zona anterior.
    // Remove from old zone's player list
    auto oldIt = m_playerZones.find(playerId);
    if (oldIt != m_playerZones.end()) {
        ZoneCoord oldZone = oldIt->second;
        if (oldZone == zone) return; // No change

        uint64_t oldKey = ZoneKey(oldZone);
        auto& oldPlayers = m_zonePlayers[oldKey];
        oldPlayers.erase(
            std::remove(oldPlayers.begin(), oldPlayers.end(), playerId),
            oldPlayers.end());
        if (oldPlayers.empty()) {
            m_zonePlayers.erase(oldKey);
        }
    }

    // ES: Añadirlo a la nueva zona.
    // Add to new zone
    m_playerZones[playerId] = zone;
    m_zonePlayers[ZoneKey(zone)].push_back(playerId);
}

// ES: Copia de la lista de jugadores de la zona.
// EN: Copy of the zone's player list.
std::vector<PlayerID> ZoneEngine::GetPlayersInZone(const ZoneCoord& zone) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_zonePlayers.find(ZoneKey(zone));
    if (it != m_zonePlayers.end()) {
        return it->second;
    }
    return {};
}

// ES: Zona del jugador, o (0,0) si no se conoce.
// EN: The player's zone, or (0,0) if unknown.
ZoneCoord ZoneEngine::GetPlayerZone(PlayerID playerId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_playerZones.find(playerId);
    if (it != m_playerZones.end()) {
        return it->second;
    }
    return ZoneCoord{0, 0};
}

// ES: Quita al jugador de ambos índices.
// EN: Removes the player from both indices.
void ZoneEngine::RemovePlayer(PlayerID playerId) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_playerZones.find(playerId);
    if (it != m_playerZones.end()) {
        uint64_t key = ZoneKey(it->second);
        auto& players = m_zonePlayers[key];
        players.erase(
            std::remove(players.begin(), players.end(), playerId),
            players.end());
        if (players.empty()) {
            m_zonePlayers.erase(key);
        }
        m_playerZones.erase(it);
    }
}

// ES: ---- Consultas de interés ----
// ---- Interest Queries ----

// ES: ¿Es la zona adyacente (o igual) a la local?
// EN: Is the zone adjacent to (or equal to) the local one?
bool ZoneEngine::IsInRange(const ZoneCoord& entityZone) const {
    return m_localZone.IsAdjacent(entityZone);
}

// ES: ¿Está la zona de la entidad en el rango de interés?
// EN: Is the entity's zone within interest range?
bool ZoneEngine::ShouldSync(EntityID entityId) const {
    auto infoCopy = m_registry.GetInfo(entityId);
    if (!infoCopy) return false;
    return m_localZone.IsAdjacent(infoCopy->zone);
}

// ES: ---- Red ----
// ---- Networking ----

// ES: Construye y envía C2S_ZoneRequest (x, y como int32) por el canal fiable ordenado.
// EN: Builds and sends C2S_ZoneRequest (x, y as int32) on the reliable ordered channel.
void ZoneEngine::NotifyZoneChange() {
    if (!m_sendFn) return;

    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_ZoneRequest);
    writer.WriteI32(m_localZone.x);
    writer.WriteI32(m_localZone.y);
    m_sendFn(writer.Data(), writer.Size(), KMP_CHANNEL_RELIABLE_ORDERED, true);

    spdlog::info("ZoneEngine: Sent zone request ({}, {}) to server", m_localZone.x, m_localZone.y);
}

// ES: ---- Reinicio ----
// ---- Reset ----

// ES: Vuelve a la zona (0,0) y vacía todos los índices.
// EN: Returns to zone (0,0) and clears all indices.
void ZoneEngine::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_localZone = ZoneCoord{0, 0};
    m_prevLocalZone = ZoneCoord{0, 0};
    m_zoneEntities.clear();
    m_playerZones.clear();
    m_zonePlayers.clear();
}

} // namespace kmp
