// ES: Registro de entidades del cliente. Es la tabla central que relaciona el id de
//     red (EntityID) de cada entidad sincronizada con su objeto real del juego
//     (puntero void* al Character/edificio de Kenshi) y guarda su estado replicado:
//     dueño, autoridad, zona, transformación, salud, equipo, etc.
//     Es thread-safe (shared_mutex: lecturas concurrentes, escrituras exclusivas),
//     porque lo usan tanto el hilo de red como el hilo del juego.
//     Se compila también en KenshiMP.UnitTest.
// EN: Client entity registry. It is the central table that maps the network id
//     (EntityID) of each synced entity to its real game object (void* pointer to the
//     Kenshi Character/building) and stores its replicated state: owner, authority,
//     zone, transform, health, equipment, etc.
//     It is thread-safe (shared_mutex: concurrent reads, exclusive writes), because
//     both the network thread and the game thread use it.
//     Also compiled into KenshiMP.UnitTest.
#pragma once
#include "kmp/types.h"
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <optional>

namespace kmp {

// ES: Toda la información que el cliente guarda de una entidad sincronizada.
// EN: All the information the client keeps about a synced entity.
struct EntityInfo {
    // ES: Identidad: id de red, generación (contador anti-"control fantasma" cuando se
    //     reutilizan ids; hoy siempre queda a 0 en el cliente), puntero al objeto del
    //     juego, tipo y jugador dueño (0 = servidor).
    // Identity
    EntityID      netId = INVALID_ENTITY;
    uint32_t      generation = 0; // NEW: Generation for ghost control prevention
    void*         gameObject = nullptr;
    EntityType    type = EntityType::NPC;
    PlayerID      ownerPlayerId = 0; // 0 = server-owned

    // ES: Estado y autoridad (spec §2.2, §2.3): ciclo de vida (Inactive/Spawning/Active...),
    //     tipo de autoridad, estado de autoridad visto desde el cliente y flags "sucios"
    //     (qué campos han cambiado y hay que replicar).
    // EN: State & authority (spec §2.2, §2.3): lifecycle (Inactive/Spawning/Active...),
    //     authority type, client-side authority state and dirty flags
    //     (which fields changed and must be replicated).
    // State & authority (spec §2.2, §2.3)
    EntityState   state     = EntityState::Inactive;
    AuthorityType authority = AuthorityType::None;
    LocalAuthorityState localState = LocalAuthorityState::RemoteOwned; // NEW: Client-side authority state
    uint16_t      dirtyFlags = Dirty_None;

    // ES: Transformación: zona de la rejilla, última posición, velocidad y rotación.
    // Transform
    ZoneCoord     zone;
    Vec3          lastPosition;
    Vec3          velocity;
    Quat          lastRotation;

    // ES: Estado de juego: salud, salud por miembro, efectos de estado, combate,
    //     velocidad de movimiento y estado de animación.
    // Game state
    float         health = 100.f;
    LimbHealth    limbs;
    uint8_t       statusEffects[5] = {}; // StatusEffectType -> active (0/1)
    CombatInfo    combat;
    uint8_t       moveSpeed = 0;
    uint8_t       animState = 0;

    // ES: Replicación: último tick actualizado, si es remota, y último equipo por
    //     ranura (14 ranuras) para detectar cambios.
    // Replication
    uint64_t      lastUpdateTick = 0;
    bool          isRemote = false; // True = controlled by another player/server
    uint32_t      lastEquipment[14] = {}; // Per-slot for diff detection
};

// ES: Registro de entidades (ver cabecera del fichero). Todas las funciones públicas
//     toman el mutex internamente.
// EN: Entity registry (see file header). All public functions take the mutex internally.
class EntityRegistry {
public:
    // ES: Registra un objeto local del juego (opcionalmente con dueño). Asigna un id
    //     local nuevo (desde 0x10000000) o devuelve el existente si ya estaba registrado.
    //     Rechaza punteros inválidos devolviendo INVALID_ENTITY.
    // Register a local game object (optionally with owner)
    EntityID Register(void* gameObject, EntityType type, PlayerID owner = 0);

    // ES: Registra una entidad remota (creada por la red) con el id que da el servidor.
    //     Queda en estado Spawning hasta que se le asocie un objeto del juego.
    // Register a remote entity (spawned by network)
    EntityID RegisterRemote(EntityID netId, EntityType type, PlayerID owner, const Vec3& pos);

    // ES: Busca el objeto del juego a partir del id de red.
    // Find game object by network ID
    void* GetGameObject(EntityID netId) const;

    // ES: Busca el id de red a partir del puntero al objeto del juego.
    // Find network ID by game object pointer
    EntityID GetNetId(void* gameObject) const;

    // ES: Devuelve una COPIA de la info de la entidad (thread-safe, sin riesgo de
    //     puntero colgante).
    // Get entity info as a copy (thread-safe — no dangling pointer risk)
    std::optional<EntityInfo> GetInfo(EntityID netId) const;

    // ES: Asocia un objeto real del juego a una entidad remota tras crearla
    //     (pasa de Spawning a Active).
    // Associate a real game object with a remote entity after spawning
    void SetGameObject(EntityID netId, void* gameObject);

    // ES: Actualiza la posición (y la zona) / la rotación registradas.
    // Update position tracking
    void UpdatePosition(EntityID netId, const Vec3& pos);
    void UpdateRotation(EntityID netId, const Quat& rot);

    // ES: Cambia el dueño de la entidad (transferencia de propiedad).
    // Update entity owner (for ownership transfer)
    void UpdateOwner(EntityID netId, PlayerID newOwner);

    // ES: Guarda el objeto equipado en una ranura (para detectar cambios).
    // Update equipment tracking for a single slot
    void UpdateEquipment(EntityID netId, int slot, uint32_t itemTemplateId);

    // ES: Actualiza la salud de los 7 miembros del cuerpo.
    // Update limb health (7 body parts)
    void UpdateLimbHealth(EntityID netId, const float health[7]);

    // ES: Activa/desactiva un efecto de estado.
    // Update a status effect flag
    void UpdateStatusEffect(EntityID netId, uint8_t effectType, bool active);

    // ES: Marca (OR) o limpia (AND-NOT) flags sucios, de forma thread-safe.
    // Update dirty flags (thread-safe bitwise OR / AND-NOT)
    void SetDirtyFlags(EntityID netId, uint16_t flags);
    void ClearDirtyFlags(EntityID netId, uint16_t mask);

    // ES: Cambia el id local de una entidad por el id nuevo asignado por el servidor.
    //     Conserva objeto del juego, dueño y todo el estado.
    // Remap a local entity ID to a new server-assigned ID.
    // Preserves the game object, owner, and all state.
    bool RemapEntityId(EntityID oldId, EntityID newId);

    // ES: Busca una entidad local (no remota) de un jugador concreto cerca de una posición.
    //     Devuelve INVALID_ENTITY si no hay ninguna.
    // Find a local (non-remote) entity near a given position owned by a specific player.
    // Returns INVALID_ENTITY if none found.
    EntityID FindLocalEntityNear(const Vec3& pos, PlayerID owner, float maxDist = 5.0f) const;

    // ES: Elimina una entidad.
    // Remove entity
    void Unregister(EntityID netId);

    // ES: Elimina todas las entidades (remotas) de una zona.
    // Remove all entities in a zone
    void RemoveEntitiesInZone(const ZoneCoord& zone);

    // ES: Todas las entidades de un jugador.
    // Get all entities owned by a player
    std::vector<EntityID> GetPlayerEntities(PlayerID playerId) const;

    // ES: Todas las entidades de una zona.
    // Get all entities in a zone
    std::vector<EntityID> GetEntitiesInZone(const ZoneCoord& zone) const;

    // ES: Todas las entidades remotas (para la interpolación).
    // Get all remote entities (for interpolation)
    std::vector<EntityID> GetRemoteEntities() const;

    // ES: Estadísticas: total, remotas y remotas ya enlazadas a un objeto del juego.
    // Stats
    size_t GetEntityCount() const;
    size_t GetRemoteCount() const;
    size_t GetSpawnedRemoteCount() const; // Remote entities with linked game objects

    // ES: Borra todas las entidades remotas (al desconectar). Devuelve cuántas.
    // Clear all remote entities (on disconnect)
    size_t ClearRemoteEntities();

    // ES: Borra todo.
    // Clear all
    void Clear();

    // ES: Ayudas para validar autoridad (Fase 1).
    // EN:
    // ── Authority Validation Helpers (Phase 1) ──

    // ES: ¿Es entidad propia (de mi jugador y en estado LocalOwned)?
    // Check if entity is locally owned (my player character)
    bool IsLocalOwned(EntityID netId, PlayerID myPlayerId) const;

    // ES: ¿Es de otro jugador (en estado RemoteOwned)?
    // Check if entity is remote-owned (another player's entity)
    bool IsRemoteOwned(EntityID netId, PlayerID myPlayerId) const;

    // ES: ¿Es del servidor (NPC/objeto del mundo, estado ServerOwned)?
    // Check if entity is server-owned (NPC/world object)
    bool IsServerOwned(EntityID netId) const;

    // ES: ¿Coincide la generación? (evita paquetes obsoletos)
    // Check if generation matches (prevents stale packets)
    bool IsValidGeneration(EntityID netId, uint32_t generation) const;

    // ES: Id del jugador dueño (INVALID_PLAYER si no existe).
    // Get entity owner player ID
    PlayerID GetOwnerPlayerId(EntityID netId) const;

    // ES: ¿Existe y es remota?
    // Check if entity exists and is remote
    bool IsRemote(EntityID netId) const;

private:
    // ES: Datos internos: mutex lector/escritor, mapa id -> info, mapa puntero -> id y
    //     siguiente id local (empieza alto para no chocar con los ids del servidor).
    // EN: Internal data: reader/writer mutex, id -> info map, pointer -> id map and
    //     next local id (starts high to avoid colliding with server-assigned ids).
    mutable std::shared_mutex m_mutex;
    std::unordered_map<EntityID, EntityInfo> m_entities;
    std::unordered_map<void*, EntityID>      m_ptrToId;
    EntityID m_nextId = 0x10000000; // Start high to avoid collisions with server-assigned IDs
};

} // namespace kmp
