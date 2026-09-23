// ES: Declaración de SpawnManager: el sistema que crea en el juego los personajes de los jugadores
//     remotos. Captura el RootObjectFactory del motor (la fábrica que instancia objetos del mundo),
//     mantiene una base de plantillas GameData (arquetipos de personaje del juego), recibe peticiones
//     de spawn desde el hilo de red y las ejecuta en el hilo del juego.
// EN: Declaration of SpawnManager: the system that creates remote players' characters in-game.
//     It captures the engine's RootObjectFactory (the factory that instantiates world objects),
//     keeps a database of GameData templates (the game's character archetypes), receives spawn
//     requests from the network thread and executes them on the game thread.

#pragma once
#include "kmp/types.h"
#include "kmp/memory.h"
#include <unordered_map>
#include <string>
#include <mutex>
#include <vector>
#include <queue>
#include <functional>
#include <atomic>

namespace kmp {

// ES: Firma de RootObjectFactory::process (RVA 0x00581770 = dirección relativa a la base de
//     kenshi_x64.exe). RCX = la fábrica, RDX = datos de entrada. Devuelve el Character* creado.
// Forward - the game's character creation function
// RootObjectFactory::process (RVA 0x00581770)
// RCX = this (factory instance), RDX = GameData* (template)
// Returns: void* (Character*)
using FactoryProcessFn = void*(__fastcall*)(void* factory, void* gameData);

// ES: Petición de spawn encolada desde el hilo de red: identidad de red, dueño, tipo, plantilla,
//     posición/rotación inicial y estado extendido (salud por parte del cuerpo, vivo/muerto).
// Pending spawn request queued from the network thread
struct SpawnRequest {
    EntityID    netId;
    PlayerID    owner;
    EntityType  type;
    std::string templateName;  // GameData template name (e.g. "Greenlander")
    Vec3        position;
    Quat        rotation;
    uint32_t    templateId;
    uint32_t    factionId;
    uint32_t    retryCount = 0; // How many times this request has been re-queued

    // Extended state (synced on connect so server+clients have correct initial state)
    bool        hasExtendedState = false;
    float       health[7] = {100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f};
    bool        alive = true;
};

// ES: Máximo de reintentos de una petición sin plantilla/factory (~10 s a 20 ticks/s).
static constexpr uint32_t MAX_SPAWN_RETRIES = 200; // ~10 seconds at 20 ticks/sec

// ── FIX-FACTORY-GW: flag de activación (2026-06-20, CORREGIDO) ──
// true  = capturar theFactory desde el PUNTERO GLOBAL theFactory (RVA 0x21345B0) sin depender
//         del hook CharacterCreate (necesario al conectar con la partida YA cargada: no hay
//         creates -> hook no dispara).
// CORRECCIÓN 2026-06-20 (RE de bytes verificado): theFactory NO está en GameWorld+0x5B0
//   (ese offset contiene un FLOAT 1.0f = 0x3F800000 -> el FIX viejo crasheaba). theFactory es un
//   PUNTERO GLOBAL independiente en .data @ RVA 0x21345B0, que 9 callers distintos de
//   RootObjectFactory::create (0x583400) cargan en rcx vía `mov rcx,[rip+theFactory]`. Su offset
//   relativo a la instancia GameWorld (modBase+0x2134110) es +0x4A0 (NO +0x5B0).
// Es VARIABLE (no constexpr) para poder DESACTIVARLO en runtime si [DIAG-FACTORY-MATCH] revela
// que el factory leído NO coincide con el que el hook captura. Definido en spawn_manager.cpp.
// EN: FIX-FACTORY-GW activation flag. true = read the factory from the GLOBAL pointer
//     theFactory (RVA 0x21345B0) without relying on the CharacterCreate hook (needed when
//     joining an already-loaded game, where no creates fire). It is NOT at GameWorld+0x5B0
//     (that holds a float 1.0f and crashed the old fix); relative to the GameWorld instance
//     (modBase+0x2134110) it is +0x4A0. Mutable so it can be switched off at runtime if
//     [DIAG-FACTORY-MATCH] shows a mismatch. Defined in spawn_manager.cpp.
extern bool kCaptureFactoryFromGameWorld;

// ES: Gestor de spawn de personajes remotos. Usado desde el hilo de red (QueueSpawn) y desde el
//     hilo del juego (hooks de creación de personaje y actualización de frame).
// EN: Remote character spawn manager. Used from the network thread (QueueSpawn) and from the
//     game thread (character-creation hooks and frame update).
class SpawnManager {
public:
    // ES: Llamado desde entity_hooks cuando el juego crea un personaje: captura la fábrica y
    //     extrae plantillas GameData del personaje recién creado.
    // Called from entity_hooks when a character is created by the game.
    // Captures the factory pointer and builds the template database.
    void OnGameCharacterCreated(void* factory, void* gameData, void* character);

    // ES: Encola una petición de spawn (thread-safe, desde el hilo de red).
    // Queue a spawn request (thread-safe, called from network thread)
    void QueueSpawn(const SpawnRequest& request);

    // ES: Procesa la cola (solo hilo del juego). OBSOLETO: la implementación actual no hace nada.
    // Process queued spawns (called from game thread only!)
    void ProcessSpawnQueue();

    // ES: Busca una plantilla GameData por nombre.
    // Find a GameData template by name
    void* FindTemplate(const std::string& name) const;

    // ES: Devuelve una plantilla de reserva cualquiera.
    // Get any valid character template (fallback)
    void* GetDefaultTemplate() const;

    // ES: ¿Tenemos ya la fábrica capturada?
    // Is the factory ready? (have we captured it yet?)
    bool IsReady() const { return m_factory != nullptr; }

    // ES: Devuelve el puntero a la fábrica (necesario para el fallback createRandomChar).
    // Get the factory pointer (needed for createRandomChar fallback)
    void* GetFactory() const { return m_factory; }

    // ES: Número de plantillas conocidas.
    // Get the number of known templates
    size_t GetTemplateCount() const;

    // ES: Escanea el heap buscando objetos GameData (se llama una vez tras cargar la partida).
    // Scan the heap for GameData objects (called once after game loads)
    void ScanGameDataHeap();

private:
    // ES: Instancia RootObjectFactory capturada (nullptr hasta capturarla).
    // The captured RootObjectFactory instance
    void* m_factory = nullptr;

    // ── FIX-FACTORY-GW (2026-06-20, CORREGIDO) ──
    // Diagnóstico de origen del factory para verificar la captura:
    //   theFactory (global RVA 0x21345B0)  ==  el `factory` (1er arg de process()) que el hook capturaba.
    // m_factoryFromGameWorld: el puntero leído del global theFactory (fix #1, sin depender del hook).
    //                         (el nombre se conserva por compat; la fuente real es el global .data).
    // m_factoryFromHook:      el 1er arg que el hook CharacterCreate recibe (SI llega a disparar).
    // Se comparan en OnGameCharacterCreated -> log [DIAG-FACTORY-MATCH]. Si difieren, el factory
    // del global NO es el correcto para CallFactoryCreate y se desactiva kCaptureFactoryFromGameWorld.
    // EN: FIX-FACTORY-GW diagnostics: m_factoryFromGameWorld = pointer read from the global
    //     theFactory (name kept for compatibility); m_factoryFromHook = first argument received
    //     by the CharacterCreate hook (if it ever fires). Compared in NoteHookFactory ->
    //     [DIAG-FACTORY-MATCH]; on mismatch the hook's one wins and the global capture is disabled.
    void* m_factoryFromGameWorld = nullptr;
    void* m_factoryFromHook = nullptr;
    bool  m_factoryMatchLogged = false; // evita spamear el log de comparación

    // ES: Puntero a la función process original (trampolín del hook: salta a la función real).
    // The original factory->process function pointer (trampoline)
    FactoryProcessFn m_origProcess = nullptr;

    // ES: Base de plantillas: nombre -> GameData*. m_defaultTemplate viene del parámetro del hook
    //     (puede ser temporal); m_characterSourcedTemplate se extrae del backpointer del personaje
    //     (persistente); m_managerPointer es el GameDataManager* detectado.
    // Template database: name -> GameData*
    mutable std::mutex m_templateMutex;
    std::unordered_map<std::string, void*> m_templates;
    void* m_defaultTemplate = nullptr; // Hook parameter template (may be invalid/temporary!)
    void* m_characterSourcedTemplate = nullptr; // Template extracted from character backpointer (validated, persistent)
    uintptr_t m_managerPointer = 0; // GameDataManager* found via character-sourced template

    // ES: Plantillas validadas por la fábrica: GameData* que el propio motor le pasó a la fábrica.
    // Factory-validated templates: these are gameData pointers actually passed to the factory
    // by the game engine. They are GUARANTEED to work with factory->process().
    // Heap-scan templates may include Race/Item/Building types that crash the factory.
    std::unordered_map<std::string, void*> m_factoryInputTemplates; // name -> gameData*
    void* m_lastFactoryInput = nullptr;       // Most recent factory input (any type)
    std::string m_lastFactoryInputName;       // Name of the above

    // ES: Plantillas de PERSONAJE: subconjunto de las anteriores cuyo objeto tiene facción.
    // CHARACTER templates: subset of factory-validated templates from objects with factions.
    // Objects with factions are actual characters (not buildings, items, or food).
    // These are the ONLY templates suitable for spawning remote player characters.
    std::unordered_map<std::string, void*> m_characterTemplates; // name -> gameData*
    void* m_lastCharacterTemplate = nullptr;
    std::string m_lastCharacterTemplateName;

    // ES: Copia del struct de petición de una llamada correcta a la fábrica (legado).
    // Saved request struct from a successful factory call
    std::vector<uint8_t> m_savedRequestStruct;
    bool m_hasRequestStruct = false;

    // ES: Datos del struct de petición capturados ANTES de la llamada, con su dirección de pila
    //     original para corregir auto-referencias (legado del spawn autónomo).
    // Pre-call request struct for standalone spawning
    uint8_t m_preCallData[1024] = {};
    size_t m_preCallDataSize = 0;
    uintptr_t m_preCallOrigAddr = 0;  // Original stack address (for self-reference fixup)
    bool m_hasPreCallData = false;

    // ES: Plantillas del mod kenshi-online.mod ('Player 1'..'Player 16'), una por slot de jugador.
    // Mod player templates: GameData* from kenshi-online.mod for each player slot.
    // These are persistent objects in the game's GameDataManager — safe to pass to factory.
    static constexpr int MAX_MOD_TEMPLATES = 16;
    void* m_modPlayerTemplates[MAX_MOD_TEMPLATES] = {};
    std::atomic<int> m_modTemplateCount{0};

    // ES: Todos los candidatos del heap con nombre 'Player N' (facción, personaje, escuadra...),
    //     para que FindModTemplates elija el de personaje por el numId.
    // Mod candidate storage: multiple GameData entries per name.
    // m_templates is a unique map (one entry per name), so when the mod defines
    // both a FACTION and a CHARACTER named "Player 1", one overwrites the other.
    // This vector stores ALL heap-scan entries for mod player names so that
    // FindModTemplates can use the numId heuristic to pick the correct one.
    std::vector<std::pair<std::string, void*>> m_modCandidates;

    // ES: Offset del GameData* dentro del struct de petición (-1 = no detectado).
    // GameData pointer offset in the request struct (detected at runtime).
    // -1 = not detected yet.
    int m_gameDataOffsetInStruct = -1;

    // ES: Offset de la posición dentro del struct de petición (-1 = no detectado).
    // Position offset in the request struct (detected at runtime by entity_hooks).
    int m_positionOffsetInStruct = -1;

    // ES: Cola de spawn: hilo de red -> hilo del juego, protegida por m_queueMutex.
    // Spawn queue (network thread -> game thread)
    mutable std::mutex m_queueMutex;
    std::queue<SpawnRequest> m_spawnQueue;

    // ES: Callback para registrar el personaje creado con su EntityID de red.
    // Callback to register spawned characters
    std::function<void(EntityID netId, void* gameObject)> m_onSpawned;

public:
    // ES: Fija la fábrica solo si aún no hay ninguna capturada.
    // EN: Sets the factory only if none has been captured yet.
    void SetFactory(void* factory) {
        if (!m_factory && factory) {
            m_factory = factory;
        }
    }

    // ── FIX-FACTORY-GW #1 (2026-06-20, CORREGIDO) ──
    // Captura theFactory desde el PUNTERO GLOBAL theFactory (RVA 0x21345B0), SIN depender del hook
    // CharacterCreate (que NO dispara al conectar con la partida ya cargada). Vía primaria: lee
    // el puntero global estático modBase+0x21345B0 (robusto). Vía fallback: GW+0x4A0 (mismo address
    // en 1.0.68; útil solo si en otra versión GameWorld fuese puntero clásico).
    // En ambas valida que el resultado es un puntero de heap con vtable en .text -> descarta el
    // float 1.0f que rompía el fix viejo (que leía el offset equivocado +0x5B0). 'gwSingleton' =
    // GameFunctions.GameWorldSingleton. Devuelve true si capturó un factory plausible.
    // EN: FIX-FACTORY-GW #1: captures theFactory from the GLOBAL pointer (RVA 0x21345B0) without
    //     depending on the CharacterCreate hook (which does not fire when joining a loaded game).
    //     Primary path: read modBase+0x21345B0. Fallback: GW+0x4A0 (same address in 1.0.68).
    //     Both validate a heap pointer whose vtable lives in the module, rejecting the float 1.0f
    //     that broke the old +0x5B0 fix. gwSingleton = GameFunctions.GameWorldSingleton.
    //     Returns true if a plausible factory was captured.
    bool CaptureFactoryFromGameWorld(uintptr_t gwSingleton);

    // [DIAG-FACTORY-MATCH] Registra el factory que el hook recibió (1er arg de process()) para
    // compararlo con el capturado de GW+0x5B0. Llamado desde OnGameCharacterCreated. No cambia
    // m_factory si éste ya fue puesto por la captura de GameWorld.
    // ES: Nota: el texto de arriba menciona GW+0x5B0 por herencia; hoy la comparación es contra el
    //     factory leído del global theFactory (RVA 0x21345B0).
    // EN: [DIAG-FACTORY-MATCH] Records the factory received by the hook (1st arg of process()) and
    //     compares it with the one read from the global theFactory (the old comment above still says
    //     GW+0x5B0). Called from OnGameCharacterCreated. Does not overwrite m_factory unless they differ.
    void NoteHookFactory(void* hookFactory);
    // ES: Setters del trampolín de process y del callback de spawn.
    // EN: Setters for the process trampoline and the spawn callback.
    void SetOrigProcess(FactoryProcessFn fn) { m_origProcess = fn; }
    void SetOnSpawnedCallback(std::function<void(EntityID, void*)> cb) { m_onSpawned = cb; }

    // ES: Guarda una copia del struct de petición de una llamada correcta de la fábrica (legado).
    // Save a copy of the factory's request struct from a successful game call.
    // The factory takes (factory*, requestStruct*), NOT (factory*, GameData*).
    // We replicate the request struct for remote spawns.
    void SetSavedRequestStruct(const uint8_t* data, size_t size);
    bool HasRequestStruct() const { return m_hasRequestStruct; }

    // ES: Guarda el struct de petición PRE-llamada con su dirección original (legado).
    // Save the PRE-CALL request struct data with original address for self-reference fixup.
    // This allows calling the factory standalone (from GameFrameUpdate) without
    // needing to be inside Hook_CharacterCreate.
    void SetPreCallData(const uint8_t* data, size_t size, uintptr_t origAddr);
    bool HasPreCallData() const { return m_hasPreCallData; }

    // ES: Spawnea un personaje. OJO: el comentario inglés de abajo describe la versión antigua
    //     (pre-call data, offset 0x20); la implementación actual delega en SpawnWithModTemplate
    //     usando la plantilla del mod del slot modSlot.
    // EN: Note: the comment below describes the old implementation; the current one delegates to
    //     SpawnWithModTemplate with the mod template of slot modSlot.
    // Spawn a character using the saved pre-call data (standalone, no hook context needed).
    // Constructs a request struct, fixes self-references, and calls the factory.
    // If desiredPosition is non-null, writes it into the struct at offset 0x20 (detected).
    // Returns the created character pointer or nullptr.
    void* SpawnCharacterDirect(const Vec3* desiredPosition = nullptr, int modSlot = 0);

    // ES: Procesa la cola desde dentro del hook CharacterCreate (hilo del juego, hook ya desactivado).
    //     Devuelve cuántos personajes se crearon.
    // Process spawn queue from within the CharacterCreate hook.
    // The hook has ALREADY disabled itself (HookBypass active), so we can call
    // the factory directly. This runs on the game thread in the correct context.
    // Returns the number of characters spawned.
    int ProcessSpawnQueueFromHook(void* factory);

    // ES: ¿Hay peticiones pendientes?
    // Check if there are pending spawn requests
    bool HasPendingSpawns() const {
        std::lock_guard lock(m_queueMutex);
        return !m_spawnQueue.empty();
    }

    // ES: Número de peticiones pendientes.
    // Get number of pending spawn requests
    size_t GetPendingSpawnCount() const {
        std::lock_guard lock(m_queueMutex);
        return m_spawnQueue.size();
    }

    // ES: Vacía la cola (al desconectarse del todo).
    // Clear all pending spawn requests (call on full disconnect)
    void ClearSpawnQueue() {
        std::lock_guard lock(m_queueMutex);
        std::queue<SpawnRequest> empty;
        m_spawnQueue.swap(empty);
    }

    // ── FIX CRASH 2º JUGADOR: reset de plantillas en reconexión ──
    // En una reconexión sin recargar el juego, las plantillas capturadas en la
    // sesión anterior (m_factory, m_modPlayerTemplates, m_factoryInputTemplates)
    // pueden apuntar a GameData liberado por el motor -> use-after-free al
    // re-spawnear. Limpiarlas fuerza una recaptura limpia. Bajo m_templateMutex
    // porque ProcessSpawnQueue (game thread) las lee concurrentemente.
    // EN: 2nd-player crash fix: template reset on reconnect. On a reconnect without reloading
    //     the game, templates captured in the previous session may point to GameData freed by
    //     the engine -> use-after-free on respawn. Clearing them forces a clean recapture.
    //     Held under m_templateMutex because ProcessSpawnQueue (game thread) reads them.
    void ResetForReconnect() {
        std::lock_guard lock(m_templateMutex);
        m_factory = nullptr;                 // se recapturará vía CaptureFactoryFromGameWorld/hook/SetFactory
        m_factoryFromGameWorld = nullptr;    // FIX-FACTORY-GW: re-armar diagnóstico en reconexión
        m_factoryFromHook = nullptr;
        m_factoryMatchLogged = false;        // permitir re-comparar hook vs GW en la nueva sesión
        m_factoryInputTemplates.clear();     // plantillas validadas por el factory (pueden estar liberadas)
        for (int i = 0; i < MAX_MOD_TEMPLATES; i++) m_modPlayerTemplates[i] = nullptr;
        m_modTemplateCount.store(0);
    }

    // ES: Quita de la cola las peticiones de un jugador (al salir). Devuelve cuántas quitó.
    // Remove pending spawn requests for a specific player (call on player leave)
    int ClearSpawnsForOwner(PlayerID owner) {
        std::lock_guard lock(m_queueMutex);
        std::queue<SpawnRequest> filtered;
        int removed = 0;
        while (!m_spawnQueue.empty()) {
            SpawnRequest req = m_spawnQueue.front();
            m_spawnQueue.pop();
            if (req.owner == owner) {
                removed++;
            } else {
                filtered.push(req);
            }
        }
        m_spawnQueue.swap(filtered);
        return removed;
    }

    // ES: Saca la siguiente petición de la cola (true si había alguna).
    // Pop the next spawn request from the queue (returns true if one was available)
    bool PopNextSpawn(SpawnRequest& outReq) {
        std::lock_guard lock(m_queueMutex);
        if (m_spawnQueue.empty()) return false;
        outReq = m_spawnQueue.front();
        m_spawnQueue.pop();
        return true;
    }

    // ES: Reencola una petición (reintento).
    // Re-queue a spawn request (for retry)
    void RequeueSpawn(const SpawnRequest& req) {
        std::lock_guard lock(m_queueMutex);
        m_spawnQueue.push(req);
    }

    // ES: Plantilla validada extraída de un personaje (preferida para spawnear).
    // Get the validated character-sourced template (preferred for spawning)
    void* GetCharacterSourcedTemplate() const {
        std::lock_guard lock(m_templateMutex);
        return m_characterSourcedTemplate;
    }

    // ES: Número de plantillas validadas por la fábrica (todos los tipos).
    // Get count of factory-validated templates (all types)
    size_t GetFactoryTemplateCount() const {
        std::lock_guard lock(m_templateMutex);
        return m_factoryInputTemplates.size();
    }

    // ES: Número de plantillas de PERSONAJE (objetos con facción).
    // Get count of CHARACTER templates (objects with factions — actual characters)
    size_t GetCharacterTemplateCount() const {
        std::lock_guard lock(m_templateMutex);
        return m_characterTemplates.size();
    }

    // ES: GameDataManager* descubierto (para arrancar el escaneo del heap).
    // Get discovered GameDataManager pointer (for heap scan bootstrapping)
    uintptr_t GetManagerPointer() const { return m_managerPointer; }

    // ES: Sistema de plantillas del mod: tras el escaneo del heap, localiza los GameData de
    //     kenshi-online.mod para los personajes de jugador (objetos persistentes y reales).
    // ── Mod Template System ──
    // After heap scan, find GameData entries from kenshi-online.mod for player characters.
    // These are REAL persistent GameData objects that the factory can use directly.
    void FindModTemplates();

    // ES: Plantilla del mod para un slot (0-based); nullptr si no existe.
    // Get mod template for a player slot (0-based). Returns nullptr if not found.
    void* GetModTemplate(int playerSlot) const;

    // ES: Número de plantillas del mod encontradas.
    // Get number of discovered mod templates
    int GetModTemplateCount() const { return m_modTemplateCount; }

    // ES: Offsets detectados en runtime dentro del struct de petición (GameData* y posición).
    // ── GameData Offset in Request Struct ──
    // Detected at runtime: offset within the pre-call request struct where
    // the GameData pointer is stored. Allows swapping templates for remote spawns.
    void SetGameDataOffset(int offset);
    int GetGameDataOffset() const { return m_gameDataOffsetInStruct; }
    void SetPositionOffset(int offset);
    int GetPositionOffset() const { return m_positionOffsetInStruct; }

    // ES: Spawn con plantilla del mod: hoy llama a RootObjectFactory::create (RVA 0x583400) con el
    //     GameData del mod y luego escribe la posición (ver el .cpp; el texto inglés es de la versión
    //     antigua que clonaba el struct).
    // ── Improved Spawn with Mod Template ──
    // Clones the pre-call struct, swaps in a mod template's GameData pointer,
    // sets desired position, fixes self-references, and calls factory.
    // This creates a fully-initialized character because the GameData is real.
    void* SpawnWithModTemplate(int playerSlot, const Vec3& position);

    // ES: Lee un std::string de Kenshi (MSVC, con SSO = cadena corta guardada en línea).
    // Read a Kenshi std::string from memory (handles SSO).
    // Public so entity_hooks and other systems can read template names.
    static std::string ReadKenshiString(uintptr_t addr);

    // ES: Comprueba y registra en el log qué vías de spawn están disponibles. Llamado desde
    //     Core::OnGameLoaded(). Devuelve true si hay al menos una.
    // Verify factory readiness and log detailed status.
    // Called from Core::OnGameLoaded() to confirm spawn system is operational.
    // Returns true if at least one spawn path is available.
    bool VerifyReadiness() const;
};

} // namespace kmp
