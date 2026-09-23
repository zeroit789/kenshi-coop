// ES: entity_manager.h - Utilidades estáticas de consulta sobre el mapa de entidades del servidor
//     (propiedad, búsquedas espaciales, límites). Se compila en el servidor pero, a fecha de este
//     comentario, server.cpp no la usa (hace estas consultas a mano).
// EN: entity_manager.h - Static query helpers over the server entity map (ownership, spatial
//     lookups, limits). It is compiled into the server but, as of this comment, server.cpp does
//     not use it (it does these queries inline).
#pragma once
#include "kmp/types.h"
#include <vector>
#include <unordered_map>

namespace kmp {

// ES: Declaración adelantada; la definición real está en server.h.
// EN: Forward declaration; the real definition lives in server.h.
struct ServerEntity; // Forward declaration from server.h

// ES: Utilidades de gestión de entidades para el servidor dedicado: consultas de propiedad,
//     búsquedas espaciales y validación. Todas estáticas y de solo lectura.
// EN: Entity management utilities for the dedicated server.
// Provides ownership queries, spatial lookups, and validation.
class EntityManager {
public:
    // ES: ── Consultas de propiedad ──
    // EN: ── Ownership queries ──

    // ES: Devuelve los IDs de todas las entidades de un jugador.
    // EN: Get all entities owned by a specific player.
    static std::vector<EntityID> GetEntitiesByOwner(
        const std::unordered_map<EntityID, ServerEntity>& entities,
        PlayerID owner);

    // ES: Cuenta las entidades de un jugador.
    // EN: Count entities owned by a specific player.
    static size_t CountByOwner(
        const std::unordered_map<EntityID, ServerEntity>& entities,
        PlayerID owner);

    // ES: Indica si un jugador es dueño de una entidad concreta.
    // EN: Check if a player owns a specific entity.
    static bool IsOwnedBy(
        const std::unordered_map<EntityID, ServerEntity>& entities,
        EntityID entityId, PlayerID playerId);

    // ES: ── Consultas espaciales ──
    // EN: ── Spatial queries ──

    // ES: Entidades de una zona exacta (misma celda de la rejilla).
    // EN: Get all entities within a zone (exact match).
    static std::vector<EntityID> GetEntitiesInZone(
        const std::unordered_map<EntityID, ServerEntity>& entities,
        ZoneCoord zone);

    // ES: Entidades de la zona y sus adyacentes (rejilla 3x3).
    // EN: Get all entities within adjacent zones (3x3 grid).
    static std::vector<EntityID> GetEntitiesNearZone(
        const std::unordered_map<EntityID, ServerEntity>& entities,
        ZoneCoord center);

    // ES: Entidades dentro de un radio alrededor de una posición del mundo.
    // EN: Get entities within a radius of a world position.
    static std::vector<EntityID> GetEntitiesInRadius(
        const std::unordered_map<EntityID, ServerEntity>& entities,
        Vec3 center, float radius);

    // ES: Entidad más cercana a una posición (opcionalmente solo de un tipo). Devuelve 0 si no hay ninguna.
    // EN: Find the nearest entity to a position (optionally filter by type).
    static EntityID FindNearest(
        const std::unordered_map<EntityID, ServerEntity>& entities,
        Vec3 position, EntityType filterType = EntityType::NPC,
        bool filterByType = false);

    // ES: ── Validación ──
    // EN: ── Validation ──

    // ES: Indica si añadir otra entidad al jugador superaría el límite por jugador (64 por defecto).
    // EN: Check if adding another entity for a player would exceed the per-player limit.
    static bool WouldExceedLimit(
        const std::unordered_map<EntityID, ServerEntity>& entities,
        PlayerID owner, size_t maxPerPlayer = 64);

    // ES: Reparto de entidades por tipo (cuántas hay de cada EntityType).
    // EN: Get entity type distribution (count per type).
    static std::unordered_map<EntityType, size_t> GetTypeDistribution(
        const std::unordered_map<EntityID, ServerEntity>& entities);
};

} // namespace kmp
