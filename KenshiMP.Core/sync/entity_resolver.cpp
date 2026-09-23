// ES: Implementación de EntityResolver (ver entity_resolver.h).
// EN: EntityResolver implementation (see entity_resolver.h).
#include "entity_resolver.h"
#include "kmp/constants.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

namespace kmp {

// ES: Constructor: solo guarda la referencia al registro.
// EN: Constructor: just stores the registry reference.
EntityResolver::EntityResolver(EntityRegistry& registry)
    : m_registry(registry) {}

// ES: ---- Comprobación interna del filtro ----
// ---- Internal Filter Matching ----

// ES: Devuelve false en cuanto un campo activado del filtro no coincide.
// EN: Returns false as soon as an enabled filter field does not match.
bool EntityResolver::MatchesFilter(const EntityInfo& info, const EntityFilter& filter) const {
    if (filter.owner != INVALID_PLAYER && info.ownerPlayerId != filter.owner)
        return false;
    if (filter.useZone && !(info.zone == filter.zone))
        return false;
    if (filter.useType && info.type != filter.type)
        return false;
    if (filter.useState && info.state != filter.state)
        return false;
    if (filter.localOnly && info.isRemote)
        return false;
    if (filter.remoteOnly && !info.isRemote)
        return false;
    if (filter.dirtyMask != Dirty_None && (info.dirtyFlags & filter.dirtyMask) == 0)
        return false;
    return true;
}

// ES: ---- Consultas compuestas ----
// ---- Compound Queries ----

// ES: Elige la consulta base más barata del registro (zona, dueño o remotas) y
//     aplica el filtro completo a cada candidato.
// EN: Picks the cheapest base query from the registry (zone, owner or remotes) and
//     applies the full filter to each candidate.
std::vector<EntityID> EntityResolver::Query(const EntityFilter& filter) const {
    std::vector<EntityID> result;

    // ES: Si se filtra por zona, partir de la consulta por zona del registro.
    // If filtering by zone, use the registry's zone query as a starting point
    if (filter.useZone) {
        auto zoneEntities = m_registry.GetEntitiesInZone(filter.zone);
        for (EntityID id : zoneEntities) {
            auto infoCopy = m_registry.GetInfo(id);
            if (infoCopy && MatchesFilter(*infoCopy, filter)) {
                result.push_back(id);
            }
        }
        return result;
    }

    // ES: Si se filtra por dueño, partir de la consulta por jugador del registro.
    // If filtering by owner, use the registry's player query
    if (filter.owner != INVALID_PLAYER) {
        auto playerEntities = m_registry.GetPlayerEntities(filter.owner);
        for (EntityID id : playerEntities) {
            auto infoCopy = m_registry.GetInfo(id);
            if (infoCopy && MatchesFilter(*infoCopy, filter)) {
                result.push_back(id);
            }
        }
        return result;
    }

    // ES: Si solo remotas, partir de la consulta de remotas del registro.
    // If filtering remote only, use the registry's remote query
    if (filter.remoteOnly) {
        auto remoteEntities = m_registry.GetRemoteEntities();
        for (EntityID id : remoteEntities) {
            auto infoCopy = m_registry.GetInfo(id);
            if (infoCopy && MatchesFilter(*infoCopy, filter)) {
                result.push_back(id);
            }
        }
        return result;
    }

    // ES: Caso general: el registro no expone una forma de recorrer todas las entidades,
    //     así que aquí solo se recorren las REMOTAS. Las locales no se incluyen sin filtro
    //     de dueño (limitación conocida, explicada abajo).
    // General case: scan all entities
    // Use GetPlayerEntities with a broad approach — iterate through what we have.
    // EntityRegistry doesn't expose a full iteration API, so we rely on zone/player queries.
    // For the general case, get all entities via GetEntityCount and known queries.
    // Since we can't iterate all entities directly, use a combination approach:
    // Get local entities (owner = any local) + remote entities
    auto remotes = m_registry.GetRemoteEntities();
    for (EntityID id : remotes) {
        auto infoCopy = m_registry.GetInfo(id);
        if (infoCopy && MatchesFilter(*infoCopy, filter)) {
            result.push_back(id);
        }
    }

    // ES: Las entidades locales no aparecen en este caso general: sin filtro de dueño
    //     habría que conocer todos los ids de jugador. Con localOnly y sin dueño el
    //     resultado sale vacío o incompleto; el que llama debe usar el filtro de dueño.
    // Also check non-remote entities that weren't in the remote list
    // Since EntityRegistry doesn't expose a full iterator, we rely on the fact that
    // local entities are tracked by player ID. For general queries without an owner filter,
    // we'd need to know all player IDs. This is a known limitation.
    // For now, the filter-by-owner and filter-by-zone paths cover the main use cases.
    // The localOnly path below uses GetPlayerEntities if a localPlayerId is known.
    if (filter.localOnly) {
        // Caller should use the owner filter for best results
        spdlog::debug("EntityResolver::Query: localOnly without owner filter — results may be incomplete");
    }

    return result;
}

// ES: Número de resultados de Query.
// EN: Number of Query results.
size_t EntityResolver::Count(const EntityFilter& filter) const {
    return Query(filter).size();
}

// ES: ---- Consultas espaciales ----
// ---- Spatial Queries ----

// ES: Toma los candidatos del vecindario 3x3 de zonas y filtra por distancia al
//     cuadrado (evita la raíz cuadrada).
// EN: Takes candidates from the 3x3 zone neighborhood and filters by squared
//     distance (avoids the square root).
std::vector<EntityID> EntityResolver::InRadius(const Vec3& center, float radius) const {
    std::vector<EntityID> result;
    float radiusSq = radius * radius;

    // ES: Entidades de la zona central y adyacentes.
    // Get entities in the center zone and adjacent zones
    ZoneCoord centerZone = ZoneCoord::FromWorldPos(center, KMP_ZONE_SIZE);
    auto candidates = InZoneNeighborhood(centerZone);

    for (EntityID id : candidates) {
        auto infoCopy = m_registry.GetInfo(id);
        if (!infoCopy) continue;
        float dx = infoCopy->lastPosition.x - center.x;
        float dy = infoCopy->lastPosition.y - center.y;
        float dz = infoCopy->lastPosition.z - center.z;
        if (dx * dx + dy * dy + dz * dz <= radiusSq) {
            result.push_back(id);
        }
    }
    return result;
}

// ES: Junta las entidades de las zonas en [-KMP_INTEREST_RADIUS, +KMP_INTEREST_RADIUS]
//     alrededor de la zona central.
// EN: Gathers the entities of the zones within [-KMP_INTEREST_RADIUS, +KMP_INTEREST_RADIUS]
//     around the center zone.
std::vector<EntityID> EntityResolver::InZoneNeighborhood(const ZoneCoord& center) const {
    std::vector<EntityID> result;
    for (int dx = -KMP_INTEREST_RADIUS; dx <= KMP_INTEREST_RADIUS; dx++) {
        for (int dy = -KMP_INTEREST_RADIUS; dy <= KMP_INTEREST_RADIUS; dy++) {
            ZoneCoord z{center.x + dx, center.y + dy};
            auto zoneEntities = m_registry.GetEntitiesInZone(z);
            result.insert(result.end(), zoneEntities.begin(), zoneEntities.end());
        }
    }
    return result;
}

// ES: ---- Gestión de interés ----
// ---- Interest Management ----

// ES: Calcula el nuevo conjunto de interés y los deltas de entrada/salida frente al
//     conjunto anterior en caché; luego actualiza la caché.
// EN: Computes the new interest set and the enter/leave deltas against the previous
//     cached set; then updates the cache.
InterestSet EntityResolver::ComputeInterest(PlayerID playerId, const ZoneCoord& playerZone) {
    InterestSet newSet;
    newSet.playerId = playerId;
    newSet.playerZone = playerZone;

    // ES: Reunir todas las entidades de la rejilla de interés 3x3.
    // Gather all entities in the 3x3 interest grid
    newSet.entities = InZoneNeighborhood(playerZone);

    // ES: Calcular entradas/salidas frente al conjunto en caché.
    // Compute enter/leave deltas against cached set
    std::lock_guard<std::mutex> lock(m_interestMutex);

    auto it = m_interestCache.find(playerId);
    if (it != m_interestCache.end()) {
        const auto& oldSet = it->second;

        // ES: Conjuntos de ids antiguos/nuevos para búsquedas rápidas.
        // Build set of old entity IDs for fast lookup
        std::unordered_set<EntityID> oldIds(oldSet.entities.begin(), oldSet.entities.end());
        std::unordered_set<EntityID> newIds(newSet.entities.begin(), newSet.entities.end());

        // ES: Entran: están en el nuevo pero no en el viejo.
        // Entering: in new but not old
        for (EntityID id : newSet.entities) {
            if (oldIds.find(id) == oldIds.end()) {
                newSet.enteringView.push_back(id);
            }
        }

        // ES: Salen: estaban en el viejo pero no en el nuevo.
        // Leaving: in old but not new
        for (EntityID id : oldSet.entities) {
            if (newIds.find(id) == newIds.end()) {
                newSet.leavingView.push_back(id);
            }
        }
    } else {
        // ES: Primer cálculo: todo "entra".
        // First computation — everything is entering
        newSet.enteringView = newSet.entities;
    }

    // ES: Guardar el nuevo conjunto en caché.
    // Cache the new set
    m_interestCache[playerId] = newSet;
    return newSet;
}

// ES: Puntero al conjunto en caché (ver aviso en el .h: válido solo mientras nadie lo cambie).
// EN: Pointer to the cached set (see the warning in the .h: valid only while nobody changes it).
const InterestSet* EntityResolver::GetInterest(PlayerID playerId) const {
    std::lock_guard<std::mutex> lock(m_interestMutex);
    auto it = m_interestCache.find(playerId);
    if (it != m_interestCache.end()) {
        return &it->second;
    }
    return nullptr;
}

// ES: Olvida la caché de interés del jugador.
// EN: Forgets the player's interest cache.
void EntityResolver::ClearInterest(PlayerID playerId) {
    std::lock_guard<std::mutex> lock(m_interestMutex);
    m_interestCache.erase(playerId);
}

// ES: ---- Gestión de flags sucios ----
// ---- Dirty Flag Management ----

// ES: Delega en el registro (OR de flags).
// EN: Delegates to the registry (flag OR).
void EntityResolver::MarkDirty(EntityID id, uint16_t flags) {
    m_registry.SetDirtyFlags(id, flags);
}

// ES: Devuelve y limpia las entidades REMOTAS con algún flag de la máscara.
//     Nota: leer y limpiar no es atómico (dos llamadas separadas al registro).
// EN: Returns and clears the REMOTE entities with any flag in the mask.
//     Note: read and clear are not atomic (two separate registry calls).
std::vector<EntityID> EntityResolver::ConsumeDirty(uint16_t mask) {
    std::vector<EntityID> result;

    // ES: Recorrer las entidades remotas buscando flags de la máscara.
    // Scan remote entities for dirty flags matching mask
    auto remotes = m_registry.GetRemoteEntities();
    for (EntityID id : remotes) {
        auto infoCopy = m_registry.GetInfo(id);
        if (infoCopy && (infoCopy->dirtyFlags & mask) != 0) {
            result.push_back(id);
            m_registry.ClearDirtyFlags(id, mask);
        }
    }
    return result;
}

// ES: ¿Tiene algún flag de la máscara?
// EN: Has any flag in the mask?
bool EntityResolver::IsDirty(EntityID id, uint16_t mask) const {
    auto infoCopy = m_registry.GetInfo(id);
    if (!infoCopy) return false;
    return (infoCopy->dirtyFlags & mask) != 0;
}

// ES: ---- Validación del ciclo de vida ----
// ---- Lifecycle Validation ----

// ES: Se puede sincronizar si está Active o Spawning.
// EN: Can be synced if Active or Spawning.
bool EntityResolver::CanSync(EntityID id) const {
    auto infoCopy = m_registry.GetInfo(id);
    if (!infoCopy) return false;
    return infoCopy->state == EntityState::Active || infoCopy->state == EntityState::Spawning;
}

// ES: Solo se puede eliminar si está Active.
// EN: Can only be despawned if Active.
bool EntityResolver::CanDespawn(EntityID id) const {
    auto infoCopy = m_registry.GetInfo(id);
    if (!infoCopy) return false;
    return infoCopy->state == EntityState::Active;
}

// ES: Viva = Active y con objeto del juego enlazado.
// EN: Alive = Active and with a linked game object.
bool EntityResolver::IsAlive(EntityID id) const {
    auto infoCopy = m_registry.GetInfo(id);
    if (!infoCopy) return false;
    return infoCopy->state == EntityState::Active && infoCopy->gameObject != nullptr;
}

// ES: ---- Propiedad ----
// ---- Ownership ----

// ES: El dueño es el jugador local indicado.
// EN: The owner is the given local player.
bool EntityResolver::IsLocallyOwned(EntityID id, PlayerID localPlayer) const {
    auto infoCopy = m_registry.GetInfo(id);
    if (!infoCopy) return false;
    return infoCopy->ownerPlayerId == localPlayer;
}

// ES: El dueño es el servidor (0).
// EN: The owner is the server (0).
bool EntityResolver::IsServerOwned(EntityID id) const {
    auto infoCopy = m_registry.GetInfo(id);
    if (!infoCopy) return false;
    return infoCopy->ownerPlayerId == 0;
}

// ES: Pasa al servidor (dueño 0) todas las entidades del jugador indicado.
// EN: Transfers every entity of the given player to the server (owner 0).
void EntityResolver::TransferToServer(PlayerID fromPlayer) {
    auto entities = m_registry.GetPlayerEntities(fromPlayer);
    for (EntityID id : entities) {
        m_registry.UpdateOwner(id, 0);
    }
    if (!entities.empty()) {
        spdlog::info("EntityResolver: Transferred {} entities from player {} to server",
                     entities.size(), fromPlayer);
    }
}

} // namespace kmp
