#pragma once
#include "kmp/types.h"
#include "server.h"

namespace kmp {

// Server-side authority validation (Phase 5)
// Ensures clients can only command entities they own
class ServerAuthorityValidator {
public:
    // Check if a client can command a specific entity
    // Returns false if:
    // - Entity doesn't exist
    // - Entity is not alive
    // - Entity authority is not Player type
    // - Entity owner doesn't match the client player ID
    static bool CanClientCommandEntity(
        PlayerID clientPlayerId,
        EntityID entityId,
        const std::unordered_map<EntityID, ServerEntity>& entities
    );

    // Batch validation for position updates (checks multiple entities at once)
    static bool ValidatePositionUpdate(
        PlayerID clientPlayerId,
        EntityID entityId,
        const std::unordered_map<EntityID, ServerEntity>& entities
    );
};

} // namespace kmp
