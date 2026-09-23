// ES: "SDK" interno de Kenshi: capa de abstracción que sondea la memoria del juego cada
//     tick (sin hooks para leer) y genera instantáneas del mundo y diferencias entre
//     ellas (qué entidades aparecen, desaparecen o cambian), más unas pocas escrituras
//     (posición, salud, nombre). Envuelve CharacterIterator, CharacterAccessor,
//     GameWorldAccessor y las cadenas de offsets de game_types.h.
// EN: Internal Kenshi "SDK": an abstraction layer that polls game memory every tick
//     (no hooks needed for reading) and produces world snapshots and diffs between them
//     (which entities appear, disappear or change), plus a few writes (position, health,
//     name). Wraps CharacterIterator, CharacterAccessor, GameWorldAccessor and the
//     offset chains from game_types.h.
#pragma once
#include "kmp/types.h"
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>
#include <mutex>

namespace kmp::sdk {

// ES: INSTANTÁNEA DE ENTIDAD: estado completo de una entidad del juego en un instante,
//     leído de la memoria por sondeo (sin hooks).
// EN:
// ═══════════════════════════════════════════════════════════════════════════
//  ENTITY SNAPSHOT
//  Complete state of one game entity at a point in time.
//  Read from game memory via polling (no hooks required).
// ═══════════════════════════════════════════════════════════════════════════

struct EntitySnapshot {
    // ES: Puntero crudo al objeto Character del juego (clave de identidad), posición,
    //     rotación, salud de las 7 partes del cuerpo, facción (puntero e ID), nombre,
    //     estado de animación, velocidad, vivo y si lo controla el jugador.
    // EN: Raw pointer to the game's Character object (identity key), position, rotation,
    //     health of the 7 body parts, faction (pointer and ID), name, animation state,
    //     speed, alive and whether the player controls it.
    uintptr_t   gamePtr    = 0;        // Raw game object pointer (identity key)
    Vec3        position;
    Quat        rotation;
    float       health[7]  = {};       // Per body part
    uintptr_t   factionPtr = 0;
    uint32_t    factionId  = 0;
    std::string name;
    uint8_t     animState  = 0;
    float       moveSpeed  = 0.f;
    bool        alive      = true;
    bool        playerControlled = false;

    // ES: Compara con la instantánea anterior y devuelve la máscara DirtyFlags de lo que cambió.
    // EN:
    // Dirty comparison: returns DirtyFlags bitmask of what changed
    uint16_t DiffAgainst(const EntitySnapshot& prev) const;
};

// ES: INSTANTÁNEA DEL MUNDO: todas las entidades + hora del día y velocidad del juego.
// EN:
// ═══════════════════════════════════════════════════════════════════════════
//  WORLD SNAPSHOT
//  All entities in the game world at a point in time.
// ═══════════════════════════════════════════════════════════════════════════

struct WorldSnapshot {
    std::vector<EntitySnapshot> entities;
    float timeOfDay  = 0.f;
    float gameSpeed  = 1.f;
    uint32_t frameNumber = 0;

    // ES: Busca una entidad por puntero del juego (nullptr si no está). Búsqueda lineal.
    // EN:
    // Find entity by game pointer (returns nullptr if not found)
    const EntitySnapshot* FindByPtr(uintptr_t ptr) const;
};

// ES: DIFERENCIA DE ESTADO: delta entre dos instantáneas del mundo (lo mínimo para sincronizar por red).
// EN:
// ═══════════════════════════════════════════════════════════════════════════
//  STATE DIFF
//  Delta between two world snapshots. Minimal data for network sync.
// ═══════════════════════════════════════════════════════════════════════════

// ES: Cambio de una entidad: su puntero, qué campos cambiaron y el estado actual completo
//     (el emisor filtra por dirtyFlags).
// EN: One entity's change: its pointer, which fields changed and the full current state
//     (the sender filters by dirtyFlags).
struct EntityDelta {
    uintptr_t gamePtr = 0;
    uint16_t  dirtyFlags = 0;     // Which fields changed (DirtyFlags bitmask)
    EntitySnapshot snapshot;       // Full current state (sender filters by dirtyFlags)
};

// ES: Diferencia del mundo: entidades cambiadas (incluye las nuevas), añadidas y eliminadas.
// EN: World diff: changed entities (including new ones), added and removed.
struct WorldDiff {
    std::vector<EntityDelta>    changed;    // Entities with state changes
    std::vector<uintptr_t>      added;      // New entities (game pointers)
    std::vector<uintptr_t>      removed;    // Entities that disappeared
};

// ES: KENSHI SDK: abstracción limpia del estado del juego. Sondea la memoria cada tick.
//     No necesita hooks para leer; envuelve CharacterIterator, CharacterAccessor,
//     GameWorldAccessor y todas las cadenas de offsets tras una API simple.
// EN:
// ═══════════════════════════════════════════════════════════════════════════
//  KENSHI SDK
//  Clean abstraction over game state. Polls game memory each tick.
//  No hooks required for reading. Wraps CharacterIterator, CharacterAccessor,
//  GameWorldAccessor, and all offset chains behind a simple API.
// ═══════════════════════════════════════════════════════════════════════════

class KenshiSDK {
public:
    KenshiSDK() = default;

    // ES: Ciclo de vida: Initialize una vez tras cargar la partida y resolver los punteros
    //     globales (PlayerBase / GameWorld).
    // EN:
    // ── Lifecycle ──
    // Call once after game is loaded and global pointers are resolved.
    bool Initialize();

    // ES: Una vez por tick de juego desde OnGameTick: sondea todas las entidades, crea la
    //     instantánea y calcula la diferencia con la anterior.
    // EN:
    // Call once per game tick from OnGameTick.
    // Polls all entities, builds snapshot, computes diff against previous.
    void Update();

    // ES: Acceso al estado (copias protegidas por mutex).
    // EN: State access (mutex-protected copies).
    // ── State Access ──

    // Get the latest world snapshot (thread-safe copy).
    WorldSnapshot GetCurrentSnapshot() const;

    // Get the diff since last Update() call.
    WorldDiff GetLastDiff() const;

    // Get snapshot for a specific entity by game pointer.
    bool GetEntityState(uintptr_t gamePtr, EntitySnapshot& out) const;

    // ES: Escritura de estado: aplicar estado remoto a una entidad (escribe en memoria del juego).
    // EN:
    // ── State Write ──
    // Apply remote state to a game entity (writes to game memory).

    bool WritePosition(uintptr_t gamePtr, const Vec3& pos);
    bool WriteHealth(uintptr_t gamePtr, BodyPart part, float value);
    bool WriteName(uintptr_t gamePtr, const std::string& name);

    // ES: Enumeración de entidades rastreadas.
    // EN:
    // ── Entity Enumeration ──

    // Get all currently tracked entity game pointers.
    std::vector<uintptr_t> GetAllEntityPtrs() const;

    // Get count of tracked entities.
    size_t GetEntityCount() const;

    // ES: Facción del jugador local (para filtrar).
    // EN:
    // ── Player Faction ──

    // Get the local player's faction pointer (for filtering).
    uintptr_t GetPlayerFactionPtr() const { return m_playerFactionPtr; }
    void SetPlayerFactionPtr(uintptr_t ptr) { m_playerFactionPtr = ptr; }

    // ES: Diagnóstico: número de frame y duración del último sondeo en ms.
    // EN: Diagnostics: frame number and duration of the last poll in ms.
    // ── Diagnostics ──
    uint32_t GetFrameNumber() const { return m_frameNumber; }
    float GetLastPollTimeMs() const { return m_lastPollTimeMs; }

    // ES: Lee el estado completo de una entidad de la memoria del juego (público para el
    //     envoltorio SEH).
    // EN:
    // Read one entity's full state from game memory (public for SEH wrapper access).
    EntitySnapshot ReadEntity(uintptr_t charPtr) const;

private:
    // ES: Sondea todos los personajes de la lista del juego.
    // EN:
    // Poll all characters from the game's character list.
    void PollEntities(WorldSnapshot& snapshot);

    // ES: Calcula la diferencia entre la instantánea vieja y la nueva.
    // EN:
    // Compute diff between old and new snapshots.
    WorldDiff ComputeDiff(const WorldSnapshot& oldSnap, const WorldSnapshot& newSnap) const;

    // ES: Mutex, instantáneas actual/anterior y última diferencia.
    // EN: Mutex, current/previous snapshots and last diff.
    mutable std::mutex m_mutex;
    WorldSnapshot m_current;
    WorldSnapshot m_previous;
    WorldDiff     m_lastDiff;

    uintptr_t m_playerFactionPtr = 0;
    uint32_t  m_frameNumber = 0;
    float     m_lastPollTimeMs = 0.f;
    bool      m_initialized = false;
};

} // namespace kmp::sdk
