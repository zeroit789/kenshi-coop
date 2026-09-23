// ES: Validador de autoridad del cliente para snapshots de posición entrantes.
//     Decide qué hacer con cada CharacterPosition recibido del servidor: aplicarlo
//     (entidad remota), reconciliar la predicción local (entidad propia), encolarlo
//     hasta que la entidad exista, o rechazarlo (generación vieja, dueño incorrecto,
//     entidad inactiva). Lo usa packet_handler.cpp al procesar las posiciones.
// EN: Client-side authority validator for inbound position snapshots.
//     Decides what to do with each CharacterPosition received from the server: apply it
//     (remote entity), reconcile local prediction (own entity), queue it until the
//     entity exists, or reject it (stale generation, wrong owner, inactive entity).
//     Used by packet_handler.cpp when processing position updates.
#pragma once
#include "kmp/types.h"
#include "kmp/messages.h"
#include "entity_registry.h"

namespace kmp {

// ES: Resultado de validar un snapshot entrante. Cada valor indica la acción a tomar.
//     Nota: RejectEcho no lo devuelve actualmente ValidateInboundSnapshot.
// EN: Result of validating an inbound snapshot. Each value tells the caller what to do.
//     Note: RejectEcho is currently never returned by ValidateInboundSnapshot.
enum class SnapshotDecision {
    ApplyRemote,             // Remote entity, valid owner → interpolate
    ReconcileLocal,          // My own entity from server → reconcile prediction
    QueuePendingSpawn,       // Entity not spawned yet → queue for later
    RejectAuthorityViolation, // Owner mismatch → reject
    RejectEcho,              // Echo of my own data → skip
    RejectStaleGeneration,   // Old generation → reject
    RejectDestroyed,         // Entity destroyed → reject
    RejectUnknown            // Unknown reason → reject
};

// ES: Clase sin estado (solo métodos estáticos) que aplica las reglas de autoridad.
// EN: Stateless class (static methods only) that applies the authority rules.
class AuthorityValidator {
public:
    // ES: Valida una actualización de posición entrante.
    //     pos: snapshot recibido; sourcePlayerId: jugador que originó el dato;
    //     myPlayerId: id del jugador local; registry: registro de entidades para
    //     consultar existencia, generación, estado y dueño. Devuelve la decisión.
    // Validate an inbound position update
    static SnapshotDecision ValidateInboundSnapshot(
        const CharacterPosition& pos,
        uint32_t sourcePlayerId,
        uint32_t myPlayerId,
        EntityRegistry& registry
    );
};

} // namespace kmp
