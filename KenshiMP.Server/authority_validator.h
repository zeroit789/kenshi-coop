// ES: authority_validator.h - Validador de autoridad del servidor dedicado.
//     Declara ServerAuthorityValidator, que comprueba que un cliente solo pueda
//     mandar órdenes (posición, movimiento, ataque) sobre entidades que le pertenecen.
//     Lo usan los handlers de GameServer (server.cpp) antes de aceptar un paquete C2S.
// EN: authority_validator.h - Authority validator for the dedicated server.
//     Declares ServerAuthorityValidator, which checks that a client can only issue
//     commands (position, movement, attack) for entities it owns.
//     Used by the GameServer handlers (server.cpp) before accepting a C2S packet.
#pragma once
#include "kmp/types.h"
#include "server.h"

namespace kmp {

// ES: Validación de autoridad en el servidor (Fase 5).
//     Garantiza que los clientes solo manden sobre entidades propias. Clase sin estado:
//     todo son funciones estáticas que reciben el mapa de entidades del servidor.
// EN: Server-side authority validation (Phase 5)
// Ensures clients can only command entities they own
class ServerAuthorityValidator {
public:
    // ES: Comprueba si un cliente puede mandar sobre una entidad concreta.
    //     Devuelve false si la entidad no existe, no está viva, su autoridad no es de tipo
    //     Player o su dueño no es el jugador cliente. Registra un aviso en cada rechazo.
    // EN: Check if a client can command a specific entity
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

    // ES: Validación para actualizaciones de posición (hoy es la misma que CanClientCommandEntity;
    //     se valida entidad a entidad aunque el comentario original hable de "batch").
    // EN: Batch validation for position updates (checks multiple entities at once)
    static bool ValidatePositionUpdate(
        PlayerID clientPlayerId,
        EntityID entityId,
        const std::unordered_map<EntityID, ServerEntity>& entities
    );
};

} // namespace kmp
