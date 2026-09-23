// ES: authority_validator.cpp - Implementación de ServerAuthorityValidator: las reglas que
//     deciden si un jugador puede controlar una entidad (existe, viva, autoridad Player, dueño).
// EN: authority_validator.cpp - ServerAuthorityValidator implementation: the rules that decide
//     whether a player may control an entity (exists, alive, Player authority, owner match).
#include "authority_validator.h"
#include <spdlog/spdlog.h>

namespace kmp {

// ES: Aplica las cuatro comprobaciones en orden y registra un warn con el motivo del rechazo.
// EN: Applies the four checks in order and logs a warning with the rejection reason.
bool ServerAuthorityValidator::CanClientCommandEntity(
    PlayerID clientPlayerId,
    EntityID entityId,
    const std::unordered_map<EntityID, ServerEntity>& entities
) {
    // ES: 1. Comprobar que la entidad existe.
    // EN: 1. Check if entity exists
    auto it = entities.find(entityId);
    if (it == entities.end()) {
        spdlog::warn("[ServerAuth] Player {} tried to command unknown entity {}",
                     clientPlayerId, entityId);
        return false;
    }

    const ServerEntity& entity = it->second;

    // ES: 2. Comprobar que la entidad está viva.
    // EN: 2. Check if entity is alive
    if (!entity.alive) {
        spdlog::warn("[ServerAuth] Player {} tried to command dead entity {}",
                     clientPlayerId, entityId);
        return false;
    }

    // ES: 3. Comprobar el tipo de autoridad (solo las entidades de jugador pueden mandarse desde un cliente).
    // EN: 3. Check authority type (only Player-owned entities can be commanded by clients)
    if (entity.authority != AuthorityType::Player) {
        spdlog::warn("[ServerAuth] Player {} tried to command non-player entity {} (authority={})",
                     clientPlayerId, entityId, static_cast<int>(entity.authority));
        return false;
    }

    // ES: 4. Comprobar que el dueño coincide con el jugador que envía la orden.
    // EN: 4. Check ownership match
    if (entity.owner != clientPlayerId) {
        spdlog::warn("[ServerAuth] AUTHORITY VIOLATION: Player {} tried to command entity {} owned by player {}",
                     clientPlayerId, entityId, entity.owner);
        return false;
    }

    return true;
}

// ES: Valida una actualización de posición; delega en CanClientCommandEntity.
// EN: Validates a position update; delegates to CanClientCommandEntity.
bool ServerAuthorityValidator::ValidatePositionUpdate(
    PlayerID clientPlayerId,
    EntityID entityId,
    const std::unordered_map<EntityID, ServerEntity>& entities
) {
    // ES: Las posiciones usan la misma validación que el resto de órdenes.
    // EN: Position updates use the same validation as other commands
    return CanClientCommandEntity(clientPlayerId, entityId, entities);
}

} // namespace kmp
