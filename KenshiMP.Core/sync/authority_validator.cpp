// ES: Implementación de AuthorityValidator: comprueba en orden existencia,
//     generación, estado activo y dueño de la entidad de un snapshot entrante.
// EN: AuthorityValidator implementation: checks, in order, existence,
//     generation, active state and owner of an inbound snapshot's entity.
#include "authority_validator.h"
#include <spdlog/spdlog.h>

namespace kmp {

// ES: Ver authority_validator.h. Las comprobaciones van numeradas (1-5) abajo.
// EN: See authority_validator.h. The checks are numbered (1-5) below.
SnapshotDecision AuthorityValidator::ValidateInboundSnapshot(
    const CharacterPosition& pos,
    uint32_t sourcePlayerId,
    uint32_t myPlayerId,
    EntityRegistry& registry
) {
    // ES: 1. Comprobar si la entidad existe en el registro.
    // 1. Check if entity exists in registry
    auto infoOpt = registry.GetInfo(pos.entityId);
    if (!infoOpt.has_value()) {
        // ES: La entidad aún no existe → encolar para que se aplique más tarde.
        // Entity not spawned yet → queue for interpolator to handle later
        return SnapshotDecision::QueuePendingSpawn;
    }
    const auto& info = infoOpt.value();

    // ES: 2. Comprobar que la generación coincide (Fase 6: ACTIVADO). Se rechazan
    //     paquetes de generaciones antiguas de la entidad (evita "fantasmas" controlados
    //     por datos de una entidad anterior con el mismo id).
    // 2. Check generation match (Phase 6: ENABLED)
    // Reject packets from old entity generations (prevents ghost control bugs)
    if (info.generation != pos.generation) {
        spdlog::debug("[AuthorityValidator] Stale generation: entity {} has gen {} but packet has gen {}",
                     pos.entityId, info.generation, pos.generation);
        return SnapshotDecision::RejectStaleGeneration;
    }

    // ES: 3. Comprobar si la entidad está inactiva.
    // 3. Check if entity is inactive
    if (info.state == EntityState::Inactive) {
        // Entity is not active → reject update
        return SnapshotDecision::RejectDestroyed;
    }

    // ES: 4. Comprobar el dueño.
    // 4. Check ownership
    uint32_t ownerPlayerId = registry.GetOwnerPlayerId(pos.entityId);

    // ES: 4a. Entidad propia que vuelve del servidor → reconciliar la predicción.
    // 4a. My own entity coming back from server → reconcile prediction
    if (ownerPlayerId == myPlayerId) {
        return SnapshotDecision::ReconcileLocal;
    }

    // ES: 4b. El origen no es dueño de esta entidad → violación de autoridad.
    // 4b. Source doesn't own this entity → authority violation
    if (ownerPlayerId != sourcePlayerId) {
        spdlog::warn(
            "[AuthorityValidator] Authority violation: entity {} owned by player {} "
            "but update came from player {}",
            pos.entityId, ownerPlayerId, sourcePlayerId
        );
        return SnapshotDecision::RejectAuthorityViolation;
    }

    // ES: 4c. Entidad remota o del servidor con dueño válido → aplicar.
    // 4c. Remote or server-owned entity with valid owner → apply
    if (registry.IsRemote(pos.entityId)) {
        return SnapshotDecision::ApplyRemote;
    }

    // ES: 5. Por defecto: motivo desconocido → rechazar (p.ej. entidad no marcada
    //     como remota aunque el dueño coincida con el origen).
    // 5. Default: unknown reason → reject
    spdlog::warn(
        "[AuthorityValidator] Unknown validation failure for entity {} from player {}",
        pos.entityId, sourcePlayerId
    );
    return SnapshotDecision::RejectUnknown;
}

} // namespace kmp
