// ES: Implementación de EntityRegistry (ver entity_registry.h).
// EN: EntityRegistry implementation (see entity_registry.h).
#include "entity_registry.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace kmp {

// ES: Registra un objeto local y le asigna un id local nuevo.
// EN: Registers a local object and assigns it a new local id.
EntityID EntityRegistry::Register(void* gameObject, EntityType type, PlayerID owner) {
    // ES: Rechaza datos de strings con SSO (small string optimization), direcciones de
    //     pila y otros punteros que no son de heap. El umbral 0x10000 (64 KB) coincide con
    //     todas las rutas de spawn/secuestro del código. También rechaza direcciones
    //     fuera del espacio de usuario x64 y no alineadas a 4 bytes.
    // Reject SSO string data, stack addresses, and other non-heap pointers.
    // Threshold 0x10000 (64KB) matches all spawn/hijack paths in the codebase.
    if (gameObject) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(gameObject);
        if (addr < 0x10000 || addr > 0x00007FFFFFFFFFFF || (addr & 0x3) != 0) {
            spdlog::error("EntityRegistry: Register REJECTED invalid pointer 0x{:X}", addr);
            return INVALID_ENTITY;
        }
    }

    std::unique_lock lock(m_mutex);

    // ES: Si ya estaba registrado, devolver su id.
    // Check if already registered
    auto it = m_ptrToId.find(gameObject);
    if (it != m_ptrToId.end()) return it->second;

    // ES: Crear la entrada: entidad local, activa, con autoridad de jugador.
    // EN: Create the entry: local entity, active, with player authority.
    EntityID id = m_nextId++;
    EntityInfo info;
    info.netId = id;
    info.gameObject = gameObject;
    info.type = type;
    info.ownerPlayerId = owner;
    info.isRemote = false;
    info.state = EntityState::Active;
    info.authority = AuthorityType::Player; // Player-owned local entity
    info.localState = LocalAuthorityState::LocalOwned;

    m_entities[id] = info;
    m_ptrToId[gameObject] = id;

    return id;
}

// ES: Registra una entidad remota con el id asignado por el servidor.
// EN: Registers a remote entity with the server-assigned id.
EntityID EntityRegistry::RegisterRemote(EntityID netId, EntityType type,
                                        PlayerID owner, const Vec3& pos) {
    std::unique_lock lock(m_mutex);

    // ES: Si ya está registrada (mensaje de spawn duplicado), no hacer nada.
    // Check if already registered (duplicate spawn message)
    auto existing = m_entities.find(netId);
    if (existing != m_entities.end()) {
        spdlog::warn("EntityRegistry: RegisterRemote called for already-registered entity {} "
                      "(state={}, hasObj={})", netId,
                      static_cast<int>(existing->second.state),
                      existing->second.gameObject != nullptr);
        return netId;
    }

    // ES: Crear la entrada: remota, en estado Spawning, sin objeto del juego todavía.
    // EN: Create the entry: remote, in Spawning state, with no game object yet.
    EntityInfo info;
    info.netId = netId;
    info.gameObject = nullptr; // Will be set when local entity is created
    info.type = type;
    info.ownerPlayerId = owner;
    info.lastPosition = pos;
    info.isRemote = true;
    info.state = EntityState::Spawning;
    info.authority = AuthorityType::Player; // Assume player-owned remote entity
    info.localState = LocalAuthorityState::RemoteOwned;
    info.zone = ZoneCoord::FromWorldPos(pos);

    m_entities[netId] = info;

    // ES: Mantener el contador de ids locales por delante de TODOS los ids conocidos.
    //     Sin max(), un id del servidor menor que m_nextId lo dejaría igual y un futuro
    //     Register() podría asignar el mismo id → colisión.
    // Ensure our local ID counter stays ahead of ALL known IDs (not just larger ones).
    // Without max(), a server-assigned ID lower than m_nextId would leave m_nextId
    // unchanged, and a future Register() could allocate the same ID → collision.
    m_nextId = std::max(m_nextId, netId + 1);

    spdlog::info("EntityRegistry: RegisterRemote entity {} (owner={}, pos=({:.0f},{:.0f},{:.0f}), state=Spawning)",
                 netId, owner, pos.x, pos.y, pos.z);

    return netId;
}

// ES: Id de red -> objeto del juego (nullptr si no existe).
// EN: Network id -> game object (nullptr if not found).
void* EntityRegistry::GetGameObject(EntityID netId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    return it != m_entities.end() ? it->second.gameObject : nullptr;
}

// ES: Objeto del juego -> id de red (INVALID_ENTITY si no existe).
// EN: Game object -> network id (INVALID_ENTITY if not found).
EntityID EntityRegistry::GetNetId(void* gameObject) const {
    std::shared_lock lock(m_mutex);
    auto it = m_ptrToId.find(gameObject);
    return it != m_ptrToId.end() ? it->second : INVALID_ENTITY;
}

// ES: Copia de la info de la entidad, o vacío si no existe.
// EN: Copy of the entity info, or empty if not found.
std::optional<EntityInfo> EntityRegistry::GetInfo(EntityID netId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it != m_entities.end()) return it->second;
    return std::nullopt;
}

// ES: Enlaza un objeto del juego a una entidad; si estaba en Spawning pasa a Active.
// EN: Links a game object to an entity; if it was Spawning it becomes Active.
void EntityRegistry::SetGameObject(EntityID netId, void* gameObject) {
    std::unique_lock lock(m_mutex);

    // ES: Rechaza datos de strings SSO, direcciones de pila y otros punteros que no son
    //     de heap (umbral 0x10000 = 64 KB, igual que en las rutas de spawn/secuestro).
    //     Los strings SSO tienen valores tipo 0x656E6F ("one"), muy por debajo de 0x10000.
    //     En ese caso se guarda nullptr en vez del puntero.
    // Reject SSO string data, stack addresses, and other non-heap pointers.
    // Threshold 0x10000 (64KB) matches all spawn/hijack paths in the codebase.
    // SSO strings have values like 0x656E6F ("one") which are well below 0x10000.
    if (gameObject) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(gameObject);
        if (addr < 0x10000 || addr > 0x00007FFFFFFFFFFF || (addr & 0x3) != 0) {
            spdlog::error("EntityRegistry: SetGameObject({}) REJECTED invalid pointer 0x{:X} "
                          "(SSO string or stack address)", netId, addr);
            gameObject = nullptr;
        }
    }

    auto it = m_entities.find(netId);
    if (it != m_entities.end()) {
        // ES: Quitar el mapeo del puntero anterior, si lo había.
        // Remove old pointer mapping if any
        if (it->second.gameObject) {
            m_ptrToId.erase(it->second.gameObject);
        }
        it->second.gameObject = gameObject;
        if (gameObject) {
            m_ptrToId[gameObject] = netId;
            // ES: Transición: Spawning → Active en cuanto se enlaza el objeto del juego.
            // Transition: Spawning → Active once game object is linked
            if (it->second.state == EntityState::Spawning) {
                it->second.state = EntityState::Active;
                spdlog::info("EntityRegistry: Entity {} transitioned Spawning -> Active "
                             "(gameObject=0x{:X}, owner={})",
                             netId, reinterpret_cast<uintptr_t>(gameObject),
                             it->second.ownerPlayerId);
            }
        }
    } else {
        spdlog::warn("EntityRegistry: SetGameObject called for unknown entity {} "
                      "(gameObject=0x{:X})", netId,
                      reinterpret_cast<uintptr_t>(gameObject));
    }
}

// ES: Guarda la última posición y recalcula la zona.
// EN: Stores the last position and recomputes the zone.
void EntityRegistry::UpdatePosition(EntityID netId, const Vec3& pos) {
    std::unique_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it != m_entities.end()) {
        it->second.lastPosition = pos;
        it->second.zone = ZoneCoord::FromWorldPos(pos);
    }
}

// ES: Guarda la última rotación.
// EN: Stores the last rotation.
void EntityRegistry::UpdateRotation(EntityID netId, const Quat& rot) {
    std::unique_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it != m_entities.end()) {
        it->second.lastRotation = rot;
    }
}

// ES: Cambia el dueño.
// EN: Changes the owner.
void EntityRegistry::UpdateOwner(EntityID netId, PlayerID newOwner) {
    std::unique_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it != m_entities.end()) {
        it->second.ownerPlayerId = newOwner;
    }
}

// ES: Guarda el objeto de una ranura de equipo (0-13).
// EN: Stores the item for one equipment slot (0-13).
void EntityRegistry::UpdateEquipment(EntityID netId, int slot, uint32_t itemTemplateId) {
    std::unique_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it != m_entities.end() && slot >= 0 && slot < 14) {
        it->second.lastEquipment[slot] = itemTemplateId;
    }
}

// ES: Copia la salud de los 7 miembros.
// EN: Copies the health of the 7 limbs.
void EntityRegistry::UpdateLimbHealth(EntityID netId, const float health[7]) {
    std::unique_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it != m_entities.end()) {
        for (int i = 0; i < 7; i++) {
            it->second.limbs.hp[i] = health[i];
        }
    }
}

// ES: Marca un efecto de estado (tipo 0-4) como activo/inactivo.
// EN: Marks a status effect (type 0-4) as active/inactive.
void EntityRegistry::UpdateStatusEffect(EntityID netId, uint8_t effectType, bool active) {
    std::unique_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it != m_entities.end() && effectType < 5) {
        it->second.statusEffects[effectType] = active ? 1 : 0;
    }
}

// ES: Añade flags sucios (OR).
// EN: Adds dirty flags (OR).
void EntityRegistry::SetDirtyFlags(EntityID netId, uint16_t flags) {
    std::unique_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it != m_entities.end()) {
        it->second.dirtyFlags |= flags;
    }
}

// ES: Limpia flags sucios (AND-NOT).
// EN: Clears dirty flags (AND-NOT).
void EntityRegistry::ClearDirtyFlags(EntityID netId, uint16_t mask) {
    std::unique_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it != m_entities.end()) {
        it->second.dirtyFlags &= ~mask;
    }
}

// ES: Mueve la entrada de oldId a newId (falla si oldId no existe o newId está ocupado).
// EN: Moves the entry from oldId to newId (fails if oldId is missing or newId is taken).
bool EntityRegistry::RemapEntityId(EntityID oldId, EntityID newId) {
    std::unique_lock lock(m_mutex);
    auto it = m_entities.find(oldId);
    if (it == m_entities.end()) return false;
    if (m_entities.count(newId) > 0) return false; // newId already taken

    EntityInfo info = it->second;
    info.netId = newId;

    // ES: Actualizar el mapeo puntero→id.
    // Update ptr→id mapping
    if (info.gameObject) {
        m_ptrToId[info.gameObject] = newId;
    }

    m_entities.erase(it);
    m_entities[newId] = info;

    // ES: Mantener el contador de ids locales por delante de todos los ids conocidos.
    // Keep our local ID counter ahead of ALL known IDs to prevent collisions.
    m_nextId = std::max(m_nextId, newId + 1);

    return true;
}

// ES: Recorre las entidades locales del dueño indicado y devuelve la más cercana
//     dentro de maxDist.
// EN: Scans the owner's local entities and returns the closest one within maxDist.
EntityID EntityRegistry::FindLocalEntityNear(const Vec3& pos, PlayerID owner, float maxDist) const {
    std::shared_lock lock(m_mutex);
    EntityID bestId = INVALID_ENTITY;
    float bestDist = maxDist;
    for (auto& [id, info] : m_entities) {
        if (info.isRemote) continue;
        if (info.ownerPlayerId != owner) continue;
        float d = info.lastPosition.DistanceTo(pos);
        if (d < bestDist) {
            bestDist = d;
            bestId = id;
        }
    }
    return bestId;
}

// ES: Borra la entidad y su mapeo de puntero.
// EN: Removes the entity and its pointer mapping.
void EntityRegistry::Unregister(EntityID netId) {
    std::unique_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it != m_entities.end()) {
        if (it->second.gameObject) {
            m_ptrToId.erase(it->second.gameObject);
        }
        m_entities.erase(it);
    }
}

// ES: Borra las entidades REMOTAS de la zona indicada (las locales se conservan).
// EN: Removes the REMOTE entities in the given zone (local ones are kept).
void EntityRegistry::RemoveEntitiesInZone(const ZoneCoord& zone) {
    std::unique_lock lock(m_mutex);
    std::vector<EntityID> toRemove;
    for (auto& [id, info] : m_entities) {
        if (info.zone == zone && info.isRemote) {
            toRemove.push_back(id);
        }
    }
    for (EntityID id : toRemove) {
        auto it = m_entities.find(id);
        if (it != m_entities.end()) {
            if (it->second.gameObject) m_ptrToId.erase(it->second.gameObject);
            m_entities.erase(it);
        }
    }
}

// ES: Lista las entidades cuyo dueño es playerId.
// EN: Lists the entities owned by playerId.
std::vector<EntityID> EntityRegistry::GetPlayerEntities(PlayerID playerId) const {
    std::shared_lock lock(m_mutex);
    std::vector<EntityID> result;
    for (auto& [id, info] : m_entities) {
        if (info.ownerPlayerId == playerId) {
            result.push_back(id);
        }
    }
    return result;
}

// ES: Lista las entidades de una zona (locales y remotas).
// EN: Lists the entities in a zone (local and remote).
std::vector<EntityID> EntityRegistry::GetEntitiesInZone(const ZoneCoord& zone) const {
    std::shared_lock lock(m_mutex);
    std::vector<EntityID> result;
    for (auto& [id, info] : m_entities) {
        if (info.zone == zone) {
            result.push_back(id);
        }
    }
    return result;
}

// ES: Lista las entidades remotas.
// EN: Lists the remote entities.
std::vector<EntityID> EntityRegistry::GetRemoteEntities() const {
    std::shared_lock lock(m_mutex);
    std::vector<EntityID> result;
    for (auto& [id, info] : m_entities) {
        if (info.isRemote) result.push_back(id);
    }
    return result;
}

// ES: Número total de entidades.
// EN: Total entity count.
size_t EntityRegistry::GetEntityCount() const {
    std::shared_lock lock(m_mutex);
    return m_entities.size();
}

// ES: Número de entidades remotas.
// EN: Remote entity count.
size_t EntityRegistry::GetRemoteCount() const {
    std::shared_lock lock(m_mutex);
    size_t count = 0;
    for (auto& [_, info] : m_entities) {
        if (info.isRemote) count++;
    }
    return count;
}

// ES: Número de entidades remotas que ya tienen objeto del juego enlazado.
// EN: Number of remote entities that already have a linked game object.
size_t EntityRegistry::GetSpawnedRemoteCount() const {
    std::shared_lock lock(m_mutex);
    size_t count = 0;
    for (auto& [_, info] : m_entities) {
        if (info.isRemote && info.gameObject != nullptr) count++;
    }
    return count;
}

// ES: Borra todas las entidades remotas y devuelve cuántas se borraron.
// EN: Removes all remote entities and returns how many were removed.
size_t EntityRegistry::ClearRemoteEntities() {
    std::unique_lock lock(m_mutex);
    std::vector<EntityID> toRemove;
    for (auto& [id, info] : m_entities) {
        if (info.isRemote) toRemove.push_back(id);
    }
    for (EntityID id : toRemove) {
        auto it = m_entities.find(id);
        if (it != m_entities.end()) {
            if (it->second.gameObject) m_ptrToId.erase(it->second.gameObject);
            m_entities.erase(it);
        }
    }
    return toRemove.size();
}

// ES: Vacía el registro por completo.
// EN: Empties the registry completely.
void EntityRegistry::Clear() {
    std::unique_lock lock(m_mutex);
    m_entities.clear();
    m_ptrToId.clear();
}

// ES: Ayudas para validar autoridad (Fase 1).
// ── Authority Validation Helpers (Phase 1) ──

// ES: Propia: dueño = mi jugador y estado local LocalOwned.
// EN: Own: owner = my player and local state LocalOwned.
bool EntityRegistry::IsLocalOwned(EntityID netId, PlayerID myPlayerId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it == m_entities.end()) return false;
    return it->second.ownerPlayerId == myPlayerId &&
           it->second.localState == LocalAuthorityState::LocalOwned;
}

// ES: De otro jugador: dueño distinto y estado local RemoteOwned.
// EN: Another player's: different owner and local state RemoteOwned.
bool EntityRegistry::IsRemoteOwned(EntityID netId, PlayerID myPlayerId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it == m_entities.end()) return false;
    return it->second.ownerPlayerId != myPlayerId &&
           it->second.localState == LocalAuthorityState::RemoteOwned;
}

// ES: Del servidor: estado local ServerOwned (ninguna ruta de este fichero lo asigna).
// EN: Server's: local state ServerOwned (no path in this file assigns it).
bool EntityRegistry::IsServerOwned(EntityID netId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it == m_entities.end()) return false;
    return it->second.localState == LocalAuthorityState::ServerOwned;
}

// ES: Compara la generación guardada con la recibida.
// EN: Compares the stored generation with the received one.
bool EntityRegistry::IsValidGeneration(EntityID netId, uint32_t generation) const {
    std::shared_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it == m_entities.end()) return false;
    return it->second.generation == generation;
}

// ES: Dueño de la entidad, o INVALID_PLAYER si no existe.
// EN: Entity owner, or INVALID_PLAYER if not found.
PlayerID EntityRegistry::GetOwnerPlayerId(EntityID netId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it == m_entities.end()) return INVALID_PLAYER;
    return it->second.ownerPlayerId;
}

// ES: true si existe y es remota.
// EN: true if it exists and is remote.
bool EntityRegistry::IsRemote(EntityID netId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_entities.find(netId);
    if (it == m_entities.end()) return false;
    return it->second.isRemote;
}

} // namespace kmp
