// ES: zone_manager.cpp - Implementación de ZoneManager (índice de población y consultas de interés).
// EN: zone_manager.cpp - ZoneManager implementation (population index and interest queries).
#include "zone_manager.h"
#include "server.h"

namespace kmp {

// ES: ── Seguimiento de población por zona ──
// EN: ── Zone population tracking ──

// ES: Vacía el índice y cuenta de nuevo cuántas entidades hay en cada zona.
// EN: Clears the index and recounts how many entities are in each zone.
void ZoneManager::RebuildIndex(
    const std::unordered_map<EntityID, ServerEntity>& entities) {

    m_zonePopulation.clear();
    for (auto& [id, entity] : entities) {
        m_zonePopulation[ZoneKey(entity.zone)]++;
    }
}

// ES: Población de una zona; 0 si no está en el índice.
// EN: Population of a zone; 0 if it is not in the index.
size_t ZoneManager::GetZonePopulation(ZoneCoord zone) const {
    auto it = m_zonePopulation.find(ZoneKey(zone));
    return it != m_zonePopulation.end() ? it->second : 0;
}

// ES: Desempaqueta las claves de 64 bits a ZoneCoord para las zonas con entidades.
// EN: Unpacks the 64-bit keys back to ZoneCoord for zones that have entities.
std::vector<ZoneCoord> ZoneManager::GetPopulatedZones() const {
    std::vector<ZoneCoord> zones;
    for (auto& [key, count] : m_zonePopulation) {
        if (count > 0) {
            int32_t x = static_cast<int32_t>(key >> 32);
            int32_t y = static_cast<int32_t>(key & 0xFFFFFFFF);
            zones.push_back(ZoneCoord(x, y));
        }
    }
    return zones;
}

// ES: ── Gestión de interés ──
// EN: ── Interest management ──

// ES: Entidades de otros jugadores/servidor en zonas adyacentes a la del jugador.
// EN: Entities of other players/the server in zones adjacent to the player's zone.
std::vector<EntityID> ZoneManager::GetRelevantEntities(
    const std::unordered_map<EntityID, ServerEntity>& entities,
    const ConnectedPlayer& player) {

    std::vector<EntityID> result;
    for (auto& [id, entity] : entities) {
        if (entity.owner == player.id) continue; // Skip own entities
        if (player.zone.IsAdjacent(entity.zone)) {
            result.push_back(id);
        }
    }
    return result;
}

// ES: Misma regla que GetRelevantEntities para una sola entidad.
// EN: Same rule as GetRelevantEntities for a single entity.
bool ZoneManager::ShouldReceiveUpdates(
    const ConnectedPlayer& player,
    const ServerEntity& entity) {

    if (entity.owner == player.id) return false;
    return player.zone.IsAdjacent(entity.zone);
}

// ES: ── Cambios de zona ──
// EN: ── Zone transitions ──

// ES: Compara las coordenadas de las dos zonas.
// EN: Compares both zones' coordinates.
bool ZoneManager::HasChangedZone(ZoneCoord oldZone, ZoneCoord newZone) {
    return oldZone.x != newZone.x || oldZone.y != newZone.y;
}

// ES: Delegado a ZoneCoord::FromWorldPos (tamaño de zona definido en kmp/types.h).
// EN: Delegates to ZoneCoord::FromWorldPos (zone size defined in kmp/types.h).
ZoneCoord ZoneManager::GetZoneForPosition(Vec3 position) {
    return ZoneCoord::FromWorldPos(position);
}

// ES: Genera la rejilla 3x3 de zonas alrededor del centro (incluido el propio centro).
// EN: Builds the 3x3 grid of zones around the center (center included).
std::vector<ZoneCoord> ZoneManager::GetAdjacentZones(ZoneCoord center) {
    std::vector<ZoneCoord> zones;
    zones.reserve(9);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            zones.push_back(ZoneCoord(center.x + dx, center.y + dy));
        }
    }
    return zones;
}

} // namespace kmp
