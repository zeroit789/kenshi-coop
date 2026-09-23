// ES: entity_manager.cpp - Implementación de EntityManager: recorridos lineales sobre el mapa
//     de entidades (O(n) por consulta). Ver entity_manager.h para la descripción de cada función.
// EN: entity_manager.cpp - EntityManager implementation: linear scans over the entity map
//     (O(n) per query). See entity_manager.h for each function's description.
#include "entity_manager.h"
#include "server.h"
#include <cmath>
#include <limits>

namespace kmp {

// ES: ── Consultas de propiedad ──
// EN: ── Ownership queries ──

// ES: Recorre todas las entidades y se queda con las del dueño indicado.
// EN: Walks all entities and keeps those owned by the given player.
std::vector<EntityID> EntityManager::GetEntitiesByOwner(
    const std::unordered_map<EntityID, ServerEntity>& entities,
    PlayerID owner) {

    std::vector<EntityID> result;
    for (auto& [id, entity] : entities) {
        if (entity.owner == owner) {
            result.push_back(id);
        }
    }
    return result;
}

// ES: Cuenta cuántas entidades tiene el dueño indicado.
// EN: Counts how many entities the given owner has.
size_t EntityManager::CountByOwner(
    const std::unordered_map<EntityID, ServerEntity>& entities,
    PlayerID owner) {

    size_t count = 0;
    for (auto& [id, entity] : entities) {
        if (entity.owner == owner) count++;
    }
    return count;
}

// ES: Busca la entidad y compara su dueño; false si no existe.
// EN: Looks the entity up and compares its owner; false if it does not exist.
bool EntityManager::IsOwnedBy(
    const std::unordered_map<EntityID, ServerEntity>& entities,
    EntityID entityId, PlayerID playerId) {

    auto it = entities.find(entityId);
    return it != entities.end() && it->second.owner == playerId;
}

// ES: ── Consultas espaciales ──
// EN: ── Spatial queries ──

// ES: Entidades cuya zona coincide exactamente con la pedida.
// EN: Entities whose zone matches the requested one exactly.
std::vector<EntityID> EntityManager::GetEntitiesInZone(
    const std::unordered_map<EntityID, ServerEntity>& entities,
    ZoneCoord zone) {

    std::vector<EntityID> result;
    for (auto& [id, entity] : entities) {
        if (entity.zone.x == zone.x && entity.zone.y == zone.y) {
            result.push_back(id);
        }
    }
    return result;
}

// ES: Entidades en la zona central o en una adyacente (usa ZoneCoord::IsAdjacent).
// EN: Entities in the center zone or an adjacent one (uses ZoneCoord::IsAdjacent).
std::vector<EntityID> EntityManager::GetEntitiesNearZone(
    const std::unordered_map<EntityID, ServerEntity>& entities,
    ZoneCoord center) {

    std::vector<EntityID> result;
    for (auto& [id, entity] : entities) {
        if (center.IsAdjacent(entity.zone)) {
            result.push_back(id);
        }
    }
    return result;
}

// ES: Entidades a distancia <= radius; compara distancias al cuadrado para evitar la raíz.
// EN: Entities within radius; compares squared distances to avoid the square root.
std::vector<EntityID> EntityManager::GetEntitiesInRadius(
    const std::unordered_map<EntityID, ServerEntity>& entities,
    Vec3 center, float radius) {

    float radiusSq = radius * radius;
    std::vector<EntityID> result;
    for (auto& [id, entity] : entities) {
        float dx = entity.position.x - center.x;
        float dy = entity.position.y - center.y;
        float dz = entity.position.z - center.z;
        if (dx * dx + dy * dy + dz * dz <= radiusSq) {
            result.push_back(id);
        }
    }
    return result;
}

// ES: Busca la entidad más cercana (filtro opcional por tipo). Devuelve 0 si el mapa está vacío
//     o ninguna pasa el filtro.
// EN: Finds the closest entity (optional type filter). Returns 0 if the map is empty or none
//     passes the filter.
EntityID EntityManager::FindNearest(
    const std::unordered_map<EntityID, ServerEntity>& entities,
    Vec3 position, EntityType filterType, bool filterByType) {

    EntityID nearest = 0;
    float nearestDistSq = std::numeric_limits<float>::max();

    for (auto& [id, entity] : entities) {
        if (filterByType && entity.type != filterType) continue;

        float dx = entity.position.x - position.x;
        float dy = entity.position.y - position.y;
        float dz = entity.position.z - position.z;
        float distSq = dx * dx + dy * dy + dz * dz;

        if (distSq < nearestDistSq) {
            nearestDistSq = distSq;
            nearest = id;
        }
    }
    return nearest;
}

// ES: ── Validación ──
// EN: ── Validation ──

// ES: true si el jugador ya tiene maxPerPlayer entidades o más.
// EN: true if the player already has maxPerPlayer entities or more.
bool EntityManager::WouldExceedLimit(
    const std::unordered_map<EntityID, ServerEntity>& entities,
    PlayerID owner, size_t maxPerPlayer) {

    return CountByOwner(entities, owner) >= maxPerPlayer;
}

// ES: Cuenta entidades por tipo (útil para estadísticas/diagnóstico).
// EN: Counts entities per type (useful for stats/diagnostics).
std::unordered_map<EntityType, size_t> EntityManager::GetTypeDistribution(
    const std::unordered_map<EntityID, ServerEntity>& entities) {

    std::unordered_map<EntityType, size_t> dist;
    for (auto& [id, entity] : entities) {
        dist[entity.type]++;
    }
    return dist;
}

} // namespace kmp
