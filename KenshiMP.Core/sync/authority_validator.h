#pragma once
#include "kmp/types.h"
#include "kmp/messages.h"
#include "entity_registry.h"

namespace kmp {

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

class AuthorityValidator {
public:
    // Validate an inbound position update
    static SnapshotDecision ValidateInboundSnapshot(
        const CharacterPosition& pos,
        uint32_t sourcePlayerId,
        uint32_t myPlayerId,
        EntityRegistry& registry
    );
};

} // namespace kmp
