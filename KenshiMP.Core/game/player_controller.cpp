// ES: player_controller.cpp - Implementación de PlayerController: inicialización del jugador
//     local, resolución del personaje primario, registro/baja de remotos, preparación de los
//     personajes remotos al spawnear (renombrado) y elección de la facción local por votación.
// EN: player_controller.cpp - PlayerController implementation: local player initialization,
//     primary character resolution, remote register/unregister, remote character setup on spawn
//     (renaming) and local faction election by voting.
#include "player_controller.h"
#include "game_types.h"
#include "spawn_manager.h"
#include "../sync/entity_registry.h"
#include "../core.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>

namespace kmp {


// ES: Guarda id y nombre del jugador local y captura la facción (char+0x10) del primer personaje
//     registrado cuyo puntero sea de heap válido (alineado a 8 y fuera del módulo del juego).
// EN: Stores the local player id and name and captures the faction (char+0x10) of the first
//     registered character whose pointer is a valid heap pointer (8-aligned, outside the game module).
void PlayerController::InitializeLocalPlayer(PlayerID localId, const std::string& playerName) {
    std::lock_guard lock(m_mutex);
    m_localPlayerId = localId;
    m_localPlayerName = playerName;
    m_initialized = true;

    spdlog::info("PlayerController: Initialized local player '{}' (ID: {})", playerName, localId);

    // ES: Capturar la facción del primer personaje local válido.
    // EN:
    // Capture faction from the first local character we can find
    auto& registry = Core::Get().GetEntityRegistry();
    auto localEntities = registry.GetPlayerEntities(localId);
    for (EntityID eid : localEntities) {
        void* gameObj = registry.GetGameObject(eid);
        if (!gameObj) continue;

        game::CharacterAccessor accessor(gameObj);
        if (!accessor.IsValid()) continue;

        uintptr_t faction = accessor.GetFactionPtr();
        if (faction > 0x10000 && faction < 0x00007FFFFFFFFFFF &&
            (faction & 0x7) == 0 && m_localFactionPtr == 0) {
            uintptr_t modBase = Memory::GetModuleBase();
            if (faction >= modBase && faction < modBase + 0x4000000) continue;
            m_localFactionPtr = faction;
            spdlog::info("PlayerController: Captured local faction ptr 0x{:X} from entity {}",
                         faction, eid);
        }
    }
}

// ES: Entidades del registro que pertenecen al jugador local.
// EN: Registry entities owned by the local player.
std::vector<EntityID> PlayerController::GetLocalSquadEntities() const {
    return Core::Get().GetEntityRegistry().GetPlayerEntities(m_localPlayerId);
}

// ES: Personaje primario del jugador local.
// EN: The local player's primary character.
void* PlayerController::GetPrimaryCharacter() const {
    // [FIX-PRIMARY 2026-07] Re-resolver SIEMPRE contra la fuente de verdad NATIVA
    // del motor: PlayerInterface+0x2B0 (lektor playerCharacters), data[0] = el
    // personaje REAL del jugador (mismo camino que ClaimHostPrimaryCharacter /
    // GetPlayerPrimaryCharacterDirect). Antes se devolvía "la primera entidad del
    // registry con game object válido": cuando el registry de red reasignaba IDs,
    // el NPC fantasma "Player N" (reclamado por nombre) podía pasar a ser "el
    // primero" y TODOS los fixes del host (facción, platoon, hostilidad) se
    // aplicaban al fantasma mientras el personaje real se quedaba congelado
    // (confirmado en vivo: ambos con activeTask=NULL, amIdle=1, char+0xDC=0).
    // EN: [FIX-PRIMARY 2026-07] Always re-resolve against the engine's NATIVE source of truth:
    //     PlayerInterface+0x2B0 (playerCharacters lektor), data[0] = the player's REAL character.
    //     Previously "the first registry entity with a game object" was returned; when the network
    //     registry reassigned ids, the ghost NPC "Player N" could become first and every host fix
    //     (faction, platoon, hostility) was applied to the ghost while the real character froze.
    uintptr_t native = game::GetPlayerPrimaryCharacterDirect();
    if (native != 0) {
        return reinterpret_cast<void*>(native);
    }

    // Fallback: si la resolución nativa falla (juego aún no cargado, lista sin
    // poblar), cae al comportamiento anterior — primera entidad del registry
    // con game object válido. NO eliminar: cubre el timing post-carga.
    // EN: Fallback: if native resolution fails (game not loaded, list not populated) use the old
    //     behavior, the first registry entity with a valid game object. Do not remove: covers post-load timing.
    auto& registry = Core::Get().GetEntityRegistry();
    auto entities = registry.GetPlayerEntities(m_localPlayerId);
    if (entities.empty()) return nullptr;

    // Return the first entity with a valid game object
    for (EntityID eid : entities) {
        void* obj = registry.GetGameObject(eid);
        if (obj) return obj;
    }
    return nullptr;
}

// ES: Registra (o actualiza) un jugador remoto; ignora el propio id local por seguridad.
// EN: Registers (or updates) a remote player; ignores our own local id as a safety measure.
void PlayerController::RegisterRemotePlayer(PlayerID id, const std::string& name) {
    // Guard against registering self as remote (defense-in-depth)
    if (id == m_localPlayerId) {
        spdlog::warn("PlayerController: Ignoring attempt to register self (ID: {}) as remote", id);
        return;
    }

    std::lock_guard lock(m_mutex);
    auto& state = m_remotePlayers[id];
    state.playerId = id;
    state.playerName = name;
    spdlog::info("PlayerController: Registered remote player '{}' (ID: {})", name, id);
}

// ES: Borra el estado del jugador remoto (sus entidades las limpia quien llama).
// EN: Deletes the remote player state (the caller cleans up its entities).
void PlayerController::RemoveRemotePlayer(PlayerID id) {
    std::lock_guard lock(m_mutex);
    auto it = m_remotePlayers.find(id);
    if (it != m_remotePlayers.end()) {
        spdlog::info("PlayerController: Removed remote player '{}' (ID: {}, {} entities)",
                     it->second.playerName, id, it->second.entities.size());
        m_remotePlayers.erase(it);
    }
}

// ES: Valida un Faction*: legible y con un primer qword que parece una vtable (dirección alta).
//     Código muerto: no se usa en ningún sitio del repo.
// EN:
// Validate faction pointer: readable and has a vtable-like first qword
static bool SEH_ValidateFaction(uintptr_t factionPtr, uintptr_t& outVtable) {
    __try {
        outVtable = *reinterpret_cast<uintptr_t*>(factionPtr);
        // Vtable should be in a valid high-address code range (module or DLL)
        return (outVtable > 0x7FF000000000ULL && outVtable < 0x7FFFFFFFFFFFF0ULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outVtable = 0;
        return false;
    }
}

// ES: Envoltorio SEH para escribir la facción en char+0x10 sin crashear si el char se liberó.
//     MSVC no permite __try junto a destructores C++ (error C2712). También sin uso en el repo.
// EN:
// SEH wrapper — __try can't coexist with C++ destructors (MSVC C2712)
static bool SEH_WriteFactionToChar(void* gameObj, uintptr_t factionPtr) {
    __try {
        game::CharacterAccessor accessor(gameObj);
        if (accessor.IsValid() && accessor.WriteFaction(factionPtr)) {
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Character may have been freed
    }
    return false;
}

// ES: Actualiza la facción local. El arreglo de facción de los personajes remotos está
//     desactivado: escribir punteros de facción de NPC causaba use-after-free al descargar la zona.
// EN: Updates the local faction. Faction fix-up of remote characters is disabled: writing NPC
//     faction pointers caused use-after-free when the source zone unloaded.
void PlayerController::SetLocalFactionPtr(uintptr_t factionPtr) {
    uintptr_t oldFaction = m_localFactionPtr;
    m_localFactionPtr = factionPtr;

    // Faction fix-up on remote characters DISABLED — writing NPC faction pointers
    // causes use-after-free crashes when the source zone unloads.
    spdlog::info("PlayerController: SetLocalFactionPtr 0x{:X} (was 0x{:X}) — fix-up disabled",
                 factionPtr, oldFaction);
}

// ES: Prepara el personaje de un remoto recién spawneado: apunta la entidad en su estado,
//     guarda su facción y lo renombra (máx 15 caracteres, cabe en el SSO de std::string).
//     La escritura de facción está desactivada (el log final aún dice "faction set").
// EN: Sets up a freshly spawned remote character: records the entity in its state, stores its
//     faction and renames it (max 15 chars, fits in std::string SSO).
//     The faction write is disabled (the final log line still says "faction set").
bool PlayerController::OnRemoteCharacterSpawned(EntityID entityId, void* gameObject, PlayerID owner) {
    if (!gameObject) return false;

    game::CharacterAccessor accessor(gameObject);
    if (!accessor.IsValid()) return false;

    // ES: Buscar el nombre del jugador remoto (bajo mutex).
    // EN:
    // Find the remote player's name
    std::string displayName;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_remotePlayers.find(owner);
        if (it != m_remotePlayers.end()) {
            displayName = it->second.playerName;
            it->second.hasSpawnedCharacter = true;
            it->second.entities.push_back(entityId);

            // Capture their faction from the first spawned character
            if (it->second.factionPtr == 0) {
                it->second.factionPtr = accessor.GetFactionPtr();
            }
        }
    }

    if (displayName.empty()) {
        displayName = "Player_" + std::to_string(owner);
    }

    // ES: 1. Renombrar el personaje con el nombre del remoto (solo el nombre de la instancia).
    // EN:
    // ── 1. Rename the character to the remote player's name ──
    // NOTE: Only the instance name is written here. GameData template name write
    // is done by the caller via RenameModCharacterSafely() because it's only
    // safe when the character was linked from a unique mod template (not
    // createRandomChar fallback which may share templates with other NPCs).
    {
        std::string safeName = displayName.substr(0, 15);
        if (accessor.WriteName(safeName)) {
            spdlog::info("PlayerController: Named entity {} -> '{}'", entityId, safeName);
        } else {
            spdlog::warn("PlayerController: WriteName failed for entity {}", entityId);
        }
    }

    // ES: 2. Escritura de facción DESACTIVADA: al descargarse la zona del NPC de origen su Faction se
    //     libera y el motor crashea leyendo faction+0x250 (en game+0x927E94). El remoto conserva la
    //     facción asignada por la fábrica.
    // EN:
    // ── 2. Faction write DISABLED ──
    // Writing a captured NPC faction pointer to the remote character causes a
    // use-after-free crash: when the source NPC's zone unloads, its faction object
    // is freed. The game engine then crashes accessing faction+0x250 on the remote
    // character (game+0x927E94, sign-extended 32-bit pointer → 0xFFFFFFFF prefix).
    // The remote character keeps its default factory-assigned faction instead.
    // TODO: Find the PLAYER's faction (not NPC faction) for allied status.
    spdlog::info("PlayerController: Faction write SKIPPED for entity {} (use-after-free prevention). "
                 "Local faction=0x{:X}", entityId, m_localFactionPtr);

    spdlog::info("PlayerController: Remote character {} set up for player '{}' (named + faction set)",
                 entityId, displayName);
    return true;
}

// ES: Escribe el nombre del remoto en la plantilla GameData del personaje (solo plantillas únicas del mod).
// EN: Writes the remote player's name into the character's GameData template (unique mod templates only).
bool PlayerController::WriteGameDataNameForModLink(void* gameObject, PlayerID owner) {
    if (!gameObject) return false;

    // Get display name
    std::string displayName;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_remotePlayers.find(owner);
        if (it != m_remotePlayers.end()) {
            displayName = it->second.playerName;
        }
    }
    if (displayName.empty()) {
        displayName = "Player_" + std::to_string(owner);
    }

    game::CharacterAccessor accessor(gameObject);
    if (!accessor.IsValid()) return false;

    std::string safeName = displayName.substr(0, 15);
    bool ok = accessor.WriteNameToGameData(safeName);
    if (ok) {
        spdlog::info("PlayerController: Wrote GameData name '{}' for mod-linked entity (owner {})",
                     safeName, owner);
    }
    return ok;
}

// ES: Devuelve un puntero al estado del remoto. OJO: el lock se suelta al salir, así que el
//     puntero puede quedar colgando si otro hilo modifica el mapa.
// EN: Returns a pointer to the remote state. NOTE: the lock is released on return, so the
//     pointer may dangle if another thread modifies the map.
const RemotePlayerState* PlayerController::GetRemotePlayer(PlayerID id) const {
    std::lock_guard lock(m_mutex);
    auto it = m_remotePlayers.find(id);
    return it != m_remotePlayers.end() ? &it->second : nullptr;
}

// ES: Copia de todos los estados remotos (seguro entre hilos).
// EN: Copy of all remote states (thread-safe).
std::vector<RemotePlayerState> PlayerController::GetAllRemotePlayers() const {
    std::lock_guard lock(m_mutex);
    std::vector<RemotePlayerState> result;
    result.reserve(m_remotePlayers.size());
    for (auto& [_, state] : m_remotePlayers) {
        result.push_back(state);
    }
    return result;
}

// ES: Punto de enganche futuro; hoy no hace nada (el bucle de sync vive en core.cpp).
// EN: Future hook point; does nothing today (the sync loop lives in core.cpp).
int PlayerController::GatherLocalEntityUpdates(float deltaTime) {
    // This is a hook point for future optimization.
    // Currently, OnGameTick in core.cpp handles the actual sync loop.
    // This method can be used to pre-filter or batch updates.
    return 0;
}

// ES: Mete la posición recibida como snapshot en el sistema de interpolación con la hora de sesión.
// EN: Pushes the received position as a snapshot into the interpolation system with session time.
void PlayerController::ApplyRemotePositionUpdate(EntityID entityId, const Vec3& pos,
                                                   const Quat& rot, uint8_t moveSpeed, uint8_t animState) {
    // Delegate to interpolation system
    float now = SessionTime();
    Core::Get().GetInterpolation().AddSnapshot(entityId, now, pos, rot, moveSpeed, animState);
}

// ES: Elige la facción local por votación entre los primeros 12 personajes: +1 por aparición,
//     +10 si el nombre coincide con el de la config y +3 si la facción es de jugador (+0x250 != 0).
//     Máx 4 candidatas. Solo fija m_localFactionPtr si aún estaba a 0.
// EN: Elects the local faction by voting over the first 12 characters: +1 per appearance,
//     +10 if the name matches the config name and +3 if it is a player faction (+0x250 != 0).
//     Max 4 candidates. Only sets m_localFactionPtr if it was still 0.
void PlayerController::OnGameWorldLoaded() {
    spdlog::info("PlayerController: Game world loaded");

    // Multi-source faction discovery: scan characters and pick the faction
    // that appears most often, with bonus for name-match and isPlayerFaction flag.
    // This handles the case where the first character is a hired NPC.

    struct FacVote { uintptr_t ptr; int score; bool nameMatch; };
    FacVote votes[4] = {};
    int voteCount = 0;
    int scanned = 0;

    const std::string& cfgName = Core::Get().GetConfig().playerName;

    game::CharacterIterator iter;
    while (iter.HasNext() && scanned < 12) {
        game::CharacterAccessor character = iter.Next();
        if (!character.IsValid()) continue;
        scanned++;

        uintptr_t faction = character.GetFactionPtr();
        if (faction < 0x10000 || faction > 0x00007FFFFFFFFFFF || (faction & 0x7) != 0) continue;

        // Reject module-internal pointers
        uintptr_t modBase = Memory::GetModuleBase();
        if (faction >= modBase && faction < modBase + 0x4000000) continue;

        // Check name match
        bool isNameMatch = false;
        if (cfgName.size() > 0) {
            std::string charName = character.GetName();
            if (charName.size() == cfgName.size() &&
                _strnicmp(charName.c_str(), cfgName.c_str(), cfgName.size()) == 0) {
                isNameMatch = true;
            }
        }

        // ES: Comprobar el flag de facción de jugador (puntero PlayerInterface* en +0x250).
        // EN:
        // Check isPlayerFaction flag
        // audit-14: isPlayerFaction (0x250) = PlayerInterface* (8 bytes), != 0 ⇒ jugador.
        // Antes se leía como bool de 1 byte (offset 0x90 erróneo) → señal basura.
        bool isFlagged = false;
        {
            const int flagOff = game::GetOffsets().faction.isPlayerFaction;
            if (flagOff >= 0) {
                uintptr_t playerIface = 0;
                Memory::Read(faction + flagOff, playerIface);
                isFlagged = (playerIface != 0);
            }
        }

        // ES: Buscar o añadir la facción en la lista de votos y sumar puntos.
        // EN:
        // Find or add to vote list
        int idx = -1;
        for (int i = 0; i < voteCount; i++) {
            if (votes[i].ptr == faction) { idx = i; break; }
        }
        if (idx < 0 && voteCount < 4) {
            idx = voteCount++;
            votes[idx] = { faction, 0, false };
        }
        if (idx >= 0) {
            votes[idx].score++;
            if (isNameMatch) { votes[idx].score += 10; votes[idx].nameMatch = true; }
            if (isFlagged) { votes[idx].score += 3; }
        }
    }

    // ES: Elegir la facción con más puntos (faction.id vale -1, así que el id del log sale 0).
    // EN:
    // Elect winner
    uintptr_t bestFaction = 0;
    int bestScore = 0;
    for (int i = 0; i < voteCount; i++) {
        if (votes[i].score > bestScore) {
            bestScore = votes[i].score;
            bestFaction = votes[i].ptr;
        }
    }

    if (bestFaction != 0 && m_localFactionPtr == 0) {
        uint32_t factionId = 0;
        const int fIdOff = game::GetOffsets().faction.id;
        if (fIdOff >= 0) Memory::Read(bestFaction + fIdOff, factionId);
        m_localFactionPtr = bestFaction;
        spdlog::info("PlayerController: Elected faction 0x{:X} (id={}) from {} chars ({} candidates)",
                     bestFaction, factionId, scanned, voteCount);
        for (int i = 0; i < voteCount; i++) {
            spdlog::info("  vote[{}]: 0x{:X} score={} nameMatch={}",
                         i, votes[i].ptr, votes[i].score, votes[i].nameMatch);
        }
    }
}

// ES: Solo registra en el log la llegada de la instantánea del mundo.
// EN: Only logs the arrival of the world snapshot.
void PlayerController::OnWorldSnapshotReceived(int entityCount) {
    spdlog::info("PlayerController: World snapshot received with {} entities", entityCount);
}

// ES: Limpia remotos y facción local (al desconectar); id y nombre local se conservan.
// EN: Clears remote players and local faction (on disconnect); local id and name are kept.
void PlayerController::Reset() {
    std::lock_guard lock(m_mutex);
    m_remotePlayers.clear();
    m_localFactionPtr = 0;
    m_initialized = false;
    spdlog::info("PlayerController: State reset");
}

} // namespace kmp
