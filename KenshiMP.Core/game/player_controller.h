// ES: player_controller.h - Controlador de jugadores: estado del jugador local (id, nombre,
//     facción, personaje primario) y de los jugadores remotos (sus personajes spawneados en
//     este cliente, renombrado, posiciones que llegan por red).
// EN: player_controller.h - Player controller: state of the local player (id, name, faction,
//     primary character) and of remote players (their characters spawned on this client,
//     renaming, positions arriving from the network).
#pragma once
#include "kmp/types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace kmp {

// ES: Declaraciones adelantadas.
// EN:
// Forward declarations
class EntityRegistry;
class SpawnManager;

// ES: Estado de un jugador remoto y de sus personajes en el mundo local.
// EN:
// Tracks a remote player's state and their characters in the world.
struct RemotePlayerState {
    PlayerID    playerId = 0;
    std::string playerName;
    Vec3        lastKnownPosition;
    bool        hasSpawnedCharacter = false;  // True if at least one character is visible
    uintptr_t   factionPtr = 0;              // Faction pointer from their first spawned character
    std::vector<EntityID> entities;           // All entity IDs owned by this player
};

// ES: PlayerController gestiona el ciclo de vida de los personajes multijugador: el local
//     (escuadra, datos que se envían) y los remotos (renombrado, facción, estado).
// EN:
// PlayerController manages the multiplayer character lifecycle:
// - Local player: tracks squad, sends entity data, handles spawn/despawn
// - Remote players: renames spawned characters, fixes factions, tracks state
class PlayerController {
public:
    // ── Local Player ──

    // ES: Se llama tras el handshake: guarda id/nombre y captura la facción del primer personaje local registrado.
    // EN:
    // Called after handshake succeeds. Scans game world for local characters
    // and registers them with the entity registry.
    void InitializeLocalPlayer(PlayerID localId, const std::string& playerName);

    // ES: Id y nombre del jugador local.
    // EN:
    // Get the local player's ID
    PlayerID GetLocalPlayerId() const { return m_localPlayerId; }
    const std::string& GetLocalPlayerName() const { return m_localPlayerName; }

    // ES: Ids de entidad de los personajes de la escuadra local.
    // EN:
    // Get entity IDs for the local player's squad characters
    std::vector<EntityID> GetLocalSquadEntities() const;

    // ES: Personaje primario del jugador local (resuelto contra la lista nativa del motor).
    // EN:
    // Get the local player's primary character (first entity, or selected)
    void* GetPrimaryCharacter() const;

    // ES: Faction* del jugador local (capturado del primer personaje).
    // EN:
    // Get the local player's faction pointer (captured from first character)
    uintptr_t GetLocalFactionPtr() const { return m_localFactionPtr; }

    // ES: Fija la Faction* local (desde entity_hooks). OJO: el arreglo de facción de los remotos
    //     que menciona el comentario inglés está DESACTIVADO en el .cpp (causaba use-after-free).
    // EN:
    // Set the local player's faction pointer (called from entity_hooks as faction bootstrap).
    // Also fixes up any previously-spawned remote characters that still have their NPC faction.
    void SetLocalFactionPtr(uintptr_t factionPtr);

    // ── Remote Players ──

    // ES: Registra un jugador remoto (al llegar PlayerJoined).
    // EN:
    // Register a remote player (called on PlayerJoined)
    void RegisterRemotePlayer(PlayerID id, const std::string& name);

    // ES: Quita un jugador remoto (al llegar PlayerLeft).
    // EN:
    // Remove a remote player and all their entities (called on PlayerLeft)
    void RemoveRemotePlayer(PlayerID id);

    // ES: Se llama cuando el personaje de un remoto aparece físicamente: lo renombra con el nombre
    //     del jugador (la escritura de facción está desactivada). True si se preparó bien.
    // EN:
    // Called when a remote player's character is physically spawned in the game world.
    // Renames the character to the player's name and fixes faction.
    // Returns true if the character was successfully set up.
    bool OnRemoteCharacterSpawned(EntityID entityId, void* gameObject, PlayerID owner);

    // ES: Escribe también el nombre en la plantilla GameData. Solo seguro con personajes enlazados a
    //     una plantilla única del mod; NO usar con createRandomChar (plantillas compartidas con NPCs).
    // EN:
    // Additionally write the display name to the GameData template.
    // Only safe for mod-linked characters where each player has a unique template.
    // Do NOT call for createRandomChar fallback — may corrupt shared NPC templates.
    bool WriteGameDataNameForModLink(void* gameObject, PlayerID owner);

    // ES: Estado de un remoto (puntero al mapa interno, válido solo mientras no cambie el mapa).
    // EN:
    // Get remote player state
    const RemotePlayerState* GetRemotePlayer(PlayerID id) const;

    // ES: Copia del estado de todos los remotos.
    // EN:
    // Get all remote player states
    std::vector<RemotePlayerState> GetAllRemotePlayers() const;

    // ── Sync ──

    // ES: Punto de enganche para recoger actualizaciones locales (hoy devuelve 0; el bucle real está en core.cpp).
    // EN:
    // Gather position/rotation data for all local entities that need syncing.
    // Returns the number of entities that had position changes.
    int GatherLocalEntityUpdates(float deltaTime);

    // ES: Aplica una posición recibida por red a una entidad remota (alimenta la interpolación).
    // EN:
    // Apply a position update to a remote entity (from network).
    // Feeds the interpolation system.
    void ApplyRemotePositionUpdate(EntityID entityId, const Vec3& pos,
                                    const Quat& rot, uint8_t moveSpeed, uint8_t animState);

    // ── Loading Flow ──

    // ES: Al terminar de cargar el mundo: elige por votación la facción del jugador local.
    // EN:
    // Called when the game world finishes loading.
    // Captures the local player's faction from their first character.
    void OnGameWorldLoaded();

    // ES: Al recibir la instantánea del mundo del servidor (hoy solo lo registra en el log).
    // EN:
    // Called when receiving a world snapshot from the server.
    // Prepares for remote entity spawning.
    void OnWorldSnapshotReceived(int entityCount);

    // ES: Reinicia el estado (al desconectar).
    // EN:
    // Reset all state (on disconnect)
    void Reset();

private:
    // ES: Mutex que protege el estado (no todos los accesos a m_localFactionPtr lo usan).
    // EN: Mutex guarding the state (not every m_localFactionPtr access uses it).
    mutable std::mutex m_mutex;

    // ES: Jugador local.
    // EN:
    // Local player
    PlayerID    m_localPlayerId = 0;
    std::string m_localPlayerName;
    uintptr_t   m_localFactionPtr = 0;  // Captured from first local character
    bool        m_initialized = false;

    // ES: Jugadores remotos indexados por PlayerID.
    // EN:
    // Remote players
    std::unordered_map<PlayerID, RemotePlayerState> m_remotePlayers;
};

} // namespace kmp
