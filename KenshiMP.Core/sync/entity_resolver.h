// ES: EntityResolver: capa de consultas por encima de EntityRegistry. Ofrece
//     consultas compuestas con filtro, consultas espaciales (radio, vecindario 3x3 de
//     zonas), gestión de interés por jugador (quién entra/sale de vista), gestión de
//     flags sucios, validación del ciclo de vida y propiedad de entidades.
// EN: EntityResolver: query layer on top of EntityRegistry. Provides compound
//     filtered queries, spatial queries (radius, 3x3 zone neighborhood), per-player
//     interest management (who enters/leaves view), dirty flag management,
//     lifecycle validation and entity ownership.
#pragma once
#include "entity_registry.h"
#include "kmp/types.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <functional>

namespace kmp {

// ES: Predicado de filtro para consultas compuestas (AND de los campos activados).
//     Ojo: el comentario de "owner" dice "0 = cualquiera", pero el código usa
//     INVALID_PLAYER como "cualquiera"; 0 filtra entidades del servidor.
// Filter predicate for compound queries (AND combination of set fields)
struct EntityFilter {
    // ES: Campos: dueño, zona (si useZone), tipo (si useType), estado (si useState),
    //     solo locales / solo remotas, y máscara de flags sucios (0 = cualquiera).
    // EN: Fields: owner, zone (if useZone), type (if useType), state (if useState),
    //     local-only / remote-only, and dirty flag mask (0 = any).
    PlayerID    owner       = INVALID_PLAYER; // 0 = any owner
    ZoneCoord   zone;
    bool        useZone     = false;
    EntityType  type        = EntityType::NPC;
    bool        useType     = false;
    EntityState state       = EntityState::Active;
    bool        useState    = false;
    bool        localOnly   = false;   // Only non-remote entities
    bool        remoteOnly  = false;   // Only remote entities
    uint16_t    dirtyMask   = Dirty_None; // 0 = any dirty state
};

// ES: Resultado de una consulta de interés: qué entidades le importan a un jugador,
//     y cuáles acaban de entrar o salir de su vista.
// Result of an interest query: which entities matter to a specific player
struct InterestSet {
    PlayerID                playerId = INVALID_PLAYER;
    ZoneCoord               playerZone;
    std::vector<EntityID>   entities;       // Entities in 3x3 zone grid
    std::vector<EntityID>   enteringView;   // Entities that just entered view
    std::vector<EntityID>   leavingView;    // Entities that just left view
};

// ES: Resolvedor de entidades. Guarda una referencia al registro y una caché de
//     interés por jugador.
// EN: Entity resolver. Holds a reference to the registry and a per-player interest cache.
class EntityResolver {
public:
    // ES: Construye el resolvedor sobre un registro existente.
    // EN: Builds the resolver on top of an existing registry.
    explicit EntityResolver(EntityRegistry& registry);

    // ES: ---- Consultas compuestas ----
    // ---- Compound Queries ----

    // ES: Entidades que cumplen todos los campos activados del filtro (AND).
    // Query entities matching all set filter fields (AND combination)
    std::vector<EntityID> Query(const EntityFilter& filter) const;

    // ES: Cuenta las entidades que cumplen el filtro (hoy llama a Query, así que sí reserva memoria).
    // Count entities matching filter (avoids allocation)
    size_t Count(const EntityFilter& filter) const;

    // ES: ---- Consultas espaciales ----
    // ---- Spatial Queries ----

    // ES: Entidades dentro de un radio alrededor de una posición del mundo.
    // Entities within radius of a world position
    std::vector<EntityID> InRadius(const Vec3& center, float radius) const;

    // ES: Entidades en una zona y todas sus adyacentes (rejilla 3x3).
    // Entities in a zone and all adjacent zones (3x3 grid)
    std::vector<EntityID> InZoneNeighborhood(const ZoneCoord& center) const;

    // ES: ---- Gestión de interés ----
    // ---- Interest Management ----

    // ES: Calcula de qué entidades debe recibir actualizaciones un jugador.
    //     Compara con el InterestSet anterior para sacar las entradas/salidas de vista.
    // Compute which entities a player should receive updates for.
    // Compares against previous InterestSet to compute enter/leave deltas.
    InterestSet ComputeInterest(PlayerID playerId, const ZoneCoord& playerZone);

    // ES: Devuelve el conjunto de interés actual de un jugador (del último ComputeInterest).
    //     Ojo: devuelve un puntero a la caché interna tras soltar el mutex; puede quedar
    //     inválido si otro hilo llama a ComputeInterest/ClearInterest.
    // EN: Note: returns a pointer into the internal cache after releasing the mutex; it
    //     may become invalid if another thread calls ComputeInterest/ClearInterest.
    // Get the current interest set for a player (from last ComputeInterest call)
    const InterestSet* GetInterest(PlayerID playerId) const;

    // ES: Borra la caché de interés de un jugador (al desconectar).
    // Clear cached interest state for a player (on disconnect)
    void ClearInterest(PlayerID playerId);

    // ES: ---- Gestión de flags sucios ----
    // ---- Dirty Flag Management ----

    // ES: Marca una entidad como sucia en ciertos campos.
    // Mark an entity as dirty for specific fields
    void MarkDirty(EntityID id, uint16_t flags);

    // ES: Devuelve las entidades sucias en al menos uno de los flags dados y limpia esos flags
    //     (solo recorre entidades remotas).
    // Get all entities dirty for at least one of the given flags, then clear those flags
    std::vector<EntityID> ConsumeDirty(uint16_t mask);

    // ES: ¿Tiene la entidad alguno de los flags sucios de la máscara?
    // Check if entity has any dirty flags set
    bool IsDirty(EntityID id, uint16_t mask) const;

    // ES: ---- Validación del ciclo de vida ----
    //     CanSync: Active o Spawning; CanDespawn: solo Active; IsAlive: Active y con objeto del juego.
    // ---- Lifecycle Validation ----

    bool CanSync(EntityID id) const;    // Active or Spawning
    bool CanDespawn(EntityID id) const; // Active only
    bool IsAlive(EntityID id) const;    // Active + has game object

    // ES: ---- Propiedad (sustituye al singleton OwnershipManager) ----
    //     IsLocallyOwned: dueño = jugador local; IsServerOwned: dueño 0;
    //     TransferToServer: pasa al servidor todas las entidades de un jugador.
    // ---- Ownership (replaces OwnershipManager singleton) ----

    bool IsLocallyOwned(EntityID id, PlayerID localPlayer) const;
    bool IsServerOwned(EntityID id) const;
    void TransferToServer(PlayerID fromPlayer);

    // ES: ---- Acceso directo al registro ----
    // ---- Direct passthrough ----
    EntityRegistry& Registry() { return m_registry; }
    const EntityRegistry& Registry() const { return m_registry; }

private:
    // ES: Comprueba si una entidad cumple el filtro.
    // EN: Checks whether an entity matches the filter.
    bool MatchesFilter(const EntityInfo& info, const EntityFilter& filter) const;

    // ES: Registro subyacente (no es propiedad de esta clase).
    // EN: Underlying registry (not owned by this class).
    EntityRegistry& m_registry;

    // ES: Cachés de interés por jugador (pequeñas: máximo 16 jugadores) y su mutex.
    // Per-player interest caches (small: max 16 players)
    std::unordered_map<PlayerID, InterestSet> m_interestCache;
    mutable std::mutex m_interestMutex;
};

} // namespace kmp
