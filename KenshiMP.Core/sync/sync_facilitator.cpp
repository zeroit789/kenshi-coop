// ES: Implementación de SyncFacilitator (ver sync_facilitator.h). Cada función comprueba
//     que esté enlazado y delega en el motor correspondiente.
// EN: SyncFacilitator implementation (see sync_facilitator.h). Each function checks it
//     is bound and delegates to the matching engine.
#include "sync_facilitator.h"
#include <spdlog/spdlog.h>

namespace kmp {

// ES: Singleton estático local.
// EN: Function-local static singleton.
SyncFacilitator& SyncFacilitator::Get() {
    static SyncFacilitator instance;
    return instance;
}

// ES: Guarda los punteros a los sistemas.
// EN: Stores pointers to the systems.
void SyncFacilitator::Bind(SyncOrchestrator* orchestrator, EntityRegistry* registry,
                            Interpolation* interpolation, SpawnManager* spawnManager) {
    m_orch = orchestrator;
    m_registry = registry;
    m_interpolation = interpolation;
    m_spawnManager = spawnManager;
    spdlog::info("SyncFacilitator: Bound to orchestrator");
}

// ES: Suelta todos los punteros.
// EN: Drops all pointers.
void SyncFacilitator::Unbind() {
    m_orch = nullptr;
    m_registry = nullptr;
    m_interpolation = nullptr;
    m_spawnManager = nullptr;
    spdlog::info("SyncFacilitator: Unbound");
}

// ES: Operaciones de entidades.
// ════════════════════════════════════════════════════════════════
// Entity Operations
// ════════════════════════════════════════════════════════════════

// ES: Relevante = su zona es adyacente a la del jugador local.
// EN: Relevant = its zone is adjacent to the local player's.
bool SyncFacilitator::IsEntityRelevant(EntityID id) const {
    if (!m_orch) return false;
    return m_orch->GetZoneEngine().ShouldSync(id);
}

// ES: Delegado a EntityResolver::IsAlive.
// EN: Delegates to EntityResolver::IsAlive.
bool SyncFacilitator::IsEntityAlive(EntityID id) const {
    if (!m_orch) return false;
    return m_orch->GetResolver().IsAlive(id);
}

// ES: Compara el dueño con el id del jugador local.
// EN: Compares the owner with the local player id.
bool SyncFacilitator::IsOwnedByLocal(EntityID id) const {
    if (!m_orch || !m_registry) return false;
    return m_orch->GetResolver().IsLocallyOwned(id,
        m_orch->GetPlayerEngine().GetLocalPlayerId());
}

// ES: Entidades en el radio; si hay filtro de dueño, se quedan solo las suyas.
//     Ojo: con filtro usa m_registry sin comprobar que no sea nulo.
// EN: Entities within the radius; with an owner filter, only theirs are kept.
//     Note: with a filter it uses m_registry without a null check.
std::vector<EntityID> SyncFacilitator::GetEntitiesNear(const Vec3& pos, float radius,
                                                         PlayerID ownerFilter) const {
    if (!m_orch) return {};

    auto entities = m_orch->GetResolver().InRadius(pos, radius);
    if (ownerFilter == INVALID_PLAYER) return entities;

    // ES: Filtrar por dueño.
    // Filter by owner
    std::vector<EntityID> filtered;
    for (EntityID id : entities) {
        auto infoCopy = m_registry->GetInfo(id);
        if (infoCopy && infoCopy->ownerPlayerId == ownerFilter) {
            filtered.push_back(id);
        }
    }
    return filtered;
}

// ES: Entidades del jugador filtradas por rango de interés.
// EN: The player's entities filtered by interest range.
std::vector<EntityID> SyncFacilitator::GetRelevantPlayerEntities(PlayerID playerId) const {
    if (!m_orch || !m_registry) return {};

    auto entities = m_registry->GetPlayerEntities(playerId);
    std::vector<EntityID> relevant;
    for (EntityID id : entities) {
        if (m_orch->GetZoneEngine().ShouldSync(id)) {
            relevant.push_back(id);
        }
    }
    return relevant;
}

// ES: Delegado a EntityResolver::MarkDirty.
// EN: Delegates to EntityResolver::MarkDirty.
void SyncFacilitator::MarkEntityDirty(EntityID id, uint16_t dirtyFlags) {
    if (!m_orch) return;
    m_orch->GetResolver().MarkDirty(id, dirtyFlags);
}

// ES: Delegado a EntityResolver::ConsumeDirty (solo entidades remotas).
// EN: Delegates to EntityResolver::ConsumeDirty (remote entities only).
std::vector<EntityID> SyncFacilitator::ConsumeDirtyEntities(uint16_t mask) {
    if (!m_orch) return {};
    return m_orch->GetResolver().ConsumeDirty(mask);
}

// ES: Operaciones de jugadores.
// ════════════════════════════════════════════════════════════════
// Player Operations
// ════════════════════════════════════════════════════════════════

// ES: Nombre local, de la sesión remota, o "Player_<id>" si no se conoce.
// EN: Local name, remote session name, or "Player_<id>" if unknown.
std::string SyncFacilitator::GetPlayerName(PlayerID id) const {
    if (!m_orch) return "";

    auto& pe = m_orch->GetPlayerEngine();

    // ES: ¿Es el jugador local?
    // Check if it's the local player
    if (id == pe.GetLocalPlayerId()) {
        return pe.GetLocalPlayerName();
    }

    // ES: Buscar entre los jugadores remotos.
    // Check remote players
    auto* session = pe.GetSession(id);
    if (session) return session->name;
    return "Player_" + std::to_string(id);
}

// ES: Zona del jugador adyacente a la local.
// EN: Player's zone adjacent to the local one.
bool SyncFacilitator::IsPlayerNearby(PlayerID id) const {
    if (!m_orch) return false;
    ZoneCoord playerZone = m_orch->GetZoneEngine().GetPlayerZone(id);
    ZoneCoord localZone = m_orch->GetZoneEngine().GetLocalZone();
    return localZone.IsAdjacent(playerZone);
}

// ES: Junta los jugadores de todas las zonas de interés, sin duplicados.
// EN: Gathers players from every interest zone, without duplicates.
std::vector<PlayerID> SyncFacilitator::GetNearbyPlayers() const {
    if (!m_orch) return {};

    std::vector<PlayerID> nearby;
    ZoneCoord localZone = m_orch->GetZoneEngine().GetLocalZone();
    auto zones = m_orch->GetZoneEngine().GetInterestZones();

    for (const auto& zone : zones) {
        auto players = m_orch->GetZoneEngine().GetPlayersInZone(zone);
        nearby.insert(nearby.end(), players.begin(), players.end());
    }

    // ES: Quitar duplicados (no debería haber, pero por si acaso).
    // Remove duplicates (shouldn't happen, but defensive)
    std::sort(nearby.begin(), nearby.end());
    nearby.erase(std::unique(nearby.begin(), nearby.end()), nearby.end());
    return nearby;
}

// ES: Delegado a PlayerEngine::GetState.
// EN: Delegates to PlayerEngine::GetState.
PlayerState SyncFacilitator::GetPlayerState(PlayerID id) const {
    if (!m_orch) return PlayerState::Disconnected;
    return m_orch->GetPlayerEngine().GetState(id);
}

// ES: Delegado a PlayerEngine::FindByName.
// EN: Delegates to PlayerEngine::FindByName.
PlayerID SyncFacilitator::FindPlayer(const std::string& partialName) const {
    if (!m_orch) return INVALID_PLAYER;
    return m_orch->GetPlayerEngine().FindByName(partialName);
}

// ES: Operaciones de zonas.
// ════════════════════════════════════════════════════════════════
// Zone Operations
// ════════════════════════════════════════════════════════════════

// ES: Zona local, o (0,0) si no está enlazado.
// EN: Local zone, or (0,0) if not bound.
ZoneCoord SyncFacilitator::GetLocalZone() const {
    if (!m_orch) return ZoneCoord{0, 0};
    return m_orch->GetZoneEngine().GetLocalZone();
}

// ES: Entidades del vecindario 3x3 de la zona local (consulta al registro, no a la caché).
// EN: Entities in the 3x3 neighborhood of the local zone (queries the registry, not the cache).
std::vector<EntityID> SyncFacilitator::GetInterestEntities() const {
    if (!m_orch) return {};

    ZoneCoord localZone = m_orch->GetZoneEngine().GetLocalZone();
    return m_orch->GetResolver().InZoneNeighborhood(localZone);
}

// ES: Población de la zona local y total/zonas pobladas en la rejilla de interés.
// EN: Population of the local zone and total/populated zones in the interest grid.
SyncFacilitator::ZoneStats SyncFacilitator::GetLocalZoneStats() const {
    ZoneStats stats = {};
    if (!m_orch) return stats;

    stats.localZone = m_orch->GetZoneEngine().GetLocalZone();
    stats.localZonePopulation = m_orch->GetZoneEngine().GetZonePopulation(stats.localZone);

    auto zones = m_orch->GetZoneEngine().GetInterestZones();
    for (const auto& z : zones) {
        size_t pop = m_orch->GetZoneEngine().GetZonePopulation(z);
        stats.interestPopulation += pop;
        if (pop > 0) stats.populatedZoneCount++;
    }
    return stats;
}

// ES: Operaciones de spawn.
// ════════════════════════════════════════════════════════════════
// Spawn Operations
// ════════════════════════════════════════════════════════════════

// ES: Delegado a SpawnManager.
// EN: Delegates to SpawnManager.
size_t SyncFacilitator::GetPendingSpawnCount() const {
    if (!m_spawnManager) return 0;
    return m_spawnManager->GetPendingSpawnCount();
}

bool SyncFacilitator::IsSpawnReady() const {
    if (!m_spawnManager) return false;
    return m_spawnManager->IsReady();
}

// ES: Notificaciones de eventos.
// ════════════════════════════════════════════════════════════════
// Event Notifications
// ════════════════════════════════════════════════════════════════

// ES: Actualiza la posición en el registro y marca la entidad como sucia en posición.
// EN: Updates the position in the registry and marks the entity position-dirty.
void SyncFacilitator::OnEntityPositionChanged(EntityID id, const Vec3& newPos) {
    if (!m_registry || !m_orch) return;
    m_registry->UpdatePosition(id, newPos);
    m_orch->GetResolver().MarkDirty(id, Dirty_Position);
}

// ES: Registra actividad del jugador (quita el estado AFK).
// EN: Records player activity (clears AFK state).
void SyncFacilitator::OnPlayerActivity(PlayerID id) {
    if (!m_orch) return;
    m_orch->GetPlayerEngine().RecordActivity(id);
}

// ES: Calcula la zona de la posición y actualiza PlayerEngine y ZoneEngine.
// EN: Computes the zone of the position and updates PlayerEngine and ZoneEngine.
void SyncFacilitator::OnPlayerPositionKnown(PlayerID id, const Vec3& pos) {
    if (!m_orch) return;
    ZoneCoord zone = ZoneCoord::FromWorldPos(pos, KMP_ZONE_SIZE);
    m_orch->GetPlayerEngine().UpdatePlayerPosition(id, pos, zone);
    m_orch->GetZoneEngine().UpdatePlayerZone(id, zone);
}

} // namespace kmp
