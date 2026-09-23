// ES: SyncFacilitator: fachada (singleton) con operaciones de alto nivel que cruzan
//     varios motores de sincronización (EntityResolver, ZoneEngine, PlayerEngine,
//     EntityRegistry, SpawnManager). Hooks, comandos y manejadores de paquetes la
//     usan para no tener que conocer cada API por separado. Solo delega, no duplica lógica.
// EN: SyncFacilitator: facade (singleton) with high-level operations that span
//     several sync engines (EntityResolver, ZoneEngine, PlayerEngine, EntityRegistry,
//     SpawnManager). Hooks, commands and packet handlers use it so they do not need
//     to know each API separately. It only delegates, no logic duplication.
#pragma once
#include "sync_orchestrator.h"
#include "entity_resolver.h"
#include "zone_engine.h"
#include "player_engine.h"
#include "entity_registry.h"
#include "interpolation.h"
#include "../game/spawn_manager.h"
#include "kmp/types.h"
#include <string>
#include <vector>

namespace kmp {

// ES: SyncFacilitator — fachada simplificada para operaciones transversales comunes.
//     En vez de que hooks, comandos y manejadores de paquetes conozcan EntityResolver,
//     ZoneEngine, PlayerEngine y sus APIs por separado, llaman al Facilitator para
//     operaciones de alto nivel (ver ejemplo abajo). Es una capa fina: guarda un puntero
//     al SyncOrchestrator y delega en el motor adecuado. Sin duplicar lógica.
//
// EN:
// SyncFacilitator — simplified facade for common cross-cutting operations.
//
// Instead of hooks, commands, and packet handlers needing to know about
// EntityResolver, ZoneEngine, PlayerEngine, and their APIs individually,
// they call the Facilitator for high-level operations.
//
// Example:
//   auto& fac = SyncFacilitator::Get();
//   fac.OnEntityPositionChanged(entityId, newPos);
//   fac.IsEntityRelevant(entityId);
//   fac.GetNearbyPlayerEntities(pos, radius);
//
// The Facilitator is a thin layer — it holds a reference to the SyncOrchestrator
// and delegates to the appropriate engine. No logic duplication.

class SyncFacilitator {
public:
    // ES: Instancia única.
    // EN: Single instance.
    static SyncFacilitator& Get();

    // ES: Hay que llamarlo una vez tras construir el SyncOrchestrator (enlaza punteros);
    //     Unbind los suelta. Mientras no esté enlazado, casi todo devuelve valores vacíos.
    // Must be called once after SyncOrchestrator is constructed
    void Bind(SyncOrchestrator* orchestrator, EntityRegistry* registry,
              Interpolation* interpolation, SpawnManager* spawnManager);
    void Unbind();
    bool IsBound() const { return m_orch != nullptr; }

    // ES: Operaciones de entidades (combina EntityResolver + Registry + Zone).
    // ════════════════════════════════════════════════════════════════
    // Entity Operations (combines EntityResolver + Registry + Zone)
    // ════════════════════════════════════════════════════════════════

    // ES: ¿Es relevante esta entidad para el jugador local (dentro del rango de interés)?
    // Is this entity relevant to the local player (in interest range)?
    bool IsEntityRelevant(EntityID id) const;

    // ES: ¿Está viva (activa + con objeto del juego)?
    // Is this entity alive (active + has game object)?
    bool IsEntityAlive(EntityID id) const;

    // ES: ¿Es del jugador local?
    // Is this entity owned by the local player?
    bool IsOwnedByLocal(EntityID id) const;

    // ES: Entidades cerca de una posición, opcionalmente filtradas por dueño.
    // Get all entities near a world position, optionally filtered by owner
    std::vector<EntityID> GetEntitiesNear(const Vec3& pos, float radius,
                                           PlayerID ownerFilter = INVALID_PLAYER) const;

    // ES: Entidades de un jugador que están en el rango de interés.
    // Get all entities owned by a player that are in the interest range
    std::vector<EntityID> GetRelevantPlayerEntities(PlayerID playerId) const;

    // ES: Marca campos de una entidad como sucios para replicarlos.
    // Mark an entity's fields as dirty for replication
    void MarkEntityDirty(EntityID id, uint16_t dirtyFlags);

    // ES: Devuelve las entidades sucias en los flags indicados y limpia esos flags.
    // Consume all entities dirty for specific flags (returns IDs, clears flags)
    std::vector<EntityID> ConsumeDirtyEntities(uint16_t mask);

    // ES: Operaciones de jugadores (combina PlayerEngine + ZoneEngine).
    // ════════════════════════════════════════════════════════════════
    // Player Operations (combines PlayerEngine + ZoneEngine)
    // ════════════════════════════════════════════════════════════════

    // ES: Nombre visible de un jugador (local o remoto).
    // Get a player's display name (works for local + remote)
    std::string GetPlayerName(PlayerID id) const;

    // ES: ¿Está el jugador remoto cerca? Ojo: el comentario original dice "misma zona",
    //     pero el código comprueba zona adyacente (rejilla 3x3).
    // Is a remote player in the same zone as the local player?
    bool IsPlayerNearby(PlayerID id) const;

    // ES: Jugadores en el vecindario de zonas del jugador local.
    // Get all players currently in the local player's zone neighborhood
    std::vector<PlayerID> GetNearbyPlayers() const;

    // ES: Estado de un jugador.
    // Get player state (Connecting, Loading, InGame, AFK, Disconnected)
    PlayerState GetPlayerState(PlayerID id) const;

    // ES: Busca un jugador por coincidencia parcial del nombre (sin distinguir mayúsculas).
    // Find a player by partial name match (case-insensitive)
    PlayerID FindPlayer(const std::string& partialName) const;

    // ES: Operaciones de zonas (usa ZoneEngine).
    // ════════════════════════════════════════════════════════════════
    // Zone Operations (combines ZoneEngine)
    // ════════════════════════════════════════════════════════════════

    // ES: Zona actual del jugador local.
    // Get the local player's current zone
    ZoneCoord GetLocalZone() const;

    // ES: Entidades en el rango de interés del jugador local (3x3 zonas).
    // Get all entities in the local player's interest range (3x3 zones)
    std::vector<EntityID> GetInterestEntities() const;

    // ES: Estadísticas de población de la zona local y alrededores (usa la caché de ZoneEngine).
    // Get zone population stats for the local area
    struct ZoneStats {
        ZoneCoord localZone;
        size_t localZonePopulation;
        size_t interestPopulation;   // Total across 3x3
        int populatedZoneCount;
    };
    ZoneStats GetLocalZoneStats() const;

    // ES: Operaciones de spawn (usa SpawnManager).
    // ════════════════════════════════════════════════════════════════
    // Spawn Operations (combines SpawnManager + EntityResolver)
    // ════════════════════════════════════════════════════════════════

    // ES: Número de spawns pendientes.
    // Get pending spawn count
    size_t GetPendingSpawnCount() const;

    // ES: ¿Está listo el sistema de spawn?
    // Is the spawn system ready?
    bool IsSpawnReady() const;

    // ES: Notificaciones de eventos (los hooks las llaman para actualizar los motores).
    // ════════════════════════════════════════════════════════════════
    // Event Notifications (hooks call these to update engines)
    // ════════════════════════════════════════════════════════════════

    // ES: Cambió la posición de una entidad (desde hooks o lectura en segundo plano).
    // Called when an entity's position changes (from hooks or background read)
    void OnEntityPositionChanged(EntityID id, const Vec3& newPos);

    // ES: Un jugador remoto envió algún paquete (seguimiento de actividad).
    // Called when a remote player sends any packet (activity tracking)
    void OnPlayerActivity(PlayerID id);

    // ES: Se conoce la posición de un jugador (desde actualizaciones de posición).
    // Called when a player's position is known (from position updates)
    void OnPlayerPositionKnown(PlayerID id, const Vec3& pos);

private:
    // ES: Constructor privado (singleton) y punteros a los sistemas enlazados.
    // EN: Private constructor (singleton) and pointers to the bound systems.
    SyncFacilitator() = default;

    SyncOrchestrator* m_orch = nullptr;
    EntityRegistry*   m_registry = nullptr;
    Interpolation*    m_interpolation = nullptr;
    SpawnManager*     m_spawnManager = nullptr;
};

} // namespace kmp
