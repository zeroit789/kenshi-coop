// ES: loading_orchestrator.h - Máquina de estados de las fases de carga del juego (carga inicial,
//     transición de zona, carga de un spawn) que decide cuándo es seguro spawnear personajes
//     remotos. Recibe eventos de los hooks de entidades (ráfagas de creación de NPCs) y,
//     opcionalmente, de hooks de recursos de Ogre.
// EN: loading_orchestrator.h - State machine of the game's loading phases (initial load, zone
//     transition, spawn load) that decides when spawning remote characters is safe. It receives
//     events from the entity hooks (NPC creation bursts) and, optionally, from Ogre resource hooks.
#pragma once
#include "kmp/types.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace kmp {

// ES: Fases del ciclo de carga; el orquestador las usa para bloquear spawns en momentos peligrosos.
// EN:
// Phases of the game's loading lifecycle.
// The orchestrator tracks these to gate spawns appropriately.
enum class LoadingPhase : uint8_t {
    Idle,            // No loading in progress — safe to spawn
    InitialLoad,     // Game first loading (save file, initial NPC burst)
    ZoneTransition,  // Player moved to new zone (navmesh + NPC loading)
    SpawnLoad,       // Resources loading for a specific character spawn
};

// ES: Estado de seguimiento de un recurso (malla, textura, material) en carga.
// EN:
// Tracking state for a single resource being loaded
struct ResourceState {
    std::string name;
    std::string type;   // "mesh", "texture", "material"
    bool loaded  = false;
    bool failed  = false;
    std::chrono::steady_clock::time_point requestTime;
    std::chrono::steady_clock::time_point completedTime;
};

// ES: LoadingOrchestrator: sigue la máquina de estados de carga y los eventos de recursos para
//     decidir el momento de spawnear. Todos los métodos públicos usan mutex/atómicos.
// EN:
// LoadingOrchestrator — tracks the game's loading state machine and resource
// loading events to gate spawn timing. Integrates with entity_hooks burst
// detection and (optionally) Ogre resource hooks for fine-grained control.
//
// Thread safety: all public methods are thread-safe (mutex + atomics).
class LoadingOrchestrator {
public:
    // ES: Consultas de fase.
    // EN: Phase queries.
    // ── Phase queries ──
    LoadingPhase GetPhase() const { return m_phase.load(std::memory_order_acquire); }
    bool IsLoading() const { return GetPhase() != LoadingPhase::Idle; }
    bool IsIdle() const { return GetPhase() == LoadingPhase::Idle; }

    // ES: Puerta de spawn: true solo si la fase es Idle, no hay recursos pendientes y ya pasó el
    //     enfriamiento desde la última carga de recurso.
    // EN:
    // ── Spawn gating ──
    // Returns true only when: phase is Idle AND no pending resources AND
    // cooldown has elapsed since the last resource load completed.
    bool IsSafeToSpawn() const;

    // ES: Pide precargar recursos de una plantilla antes del spawn (hoy es un no-op).
    // EN:
    // Request preloading of resources for a template (anticipatory, before spawn)
    void RequestPreload(const std::string& templateName);

    // ES: Seguimiento de recursos (lo llaman los hooks de recursos de Ogre).
    // EN:
    // ── Resource tracking (called from Ogre resource hooks) ──
    void OnResourceRequested(const std::string& name, const std::string& type);
    void OnResourceLoaded(const std::string& name, const std::string& type);
    void OnResourceFailed(const std::string& name, const std::string& type);

    // ES: Integración con la detección de ráfagas de creación de personajes (entity_hooks).
    // EN:
    // ── Burst detection integration (called from entity_hooks) ──
    void OnBurstDetected(int createCount);
    void OnBurstEnded(int totalCreates);

    // ES: Eventos del ciclo de vida: partida cargada, inicio y fin de carga de zona.
    // EN:
    // ── Lifecycle events ──
    void OnGameLoaded();
    void OnZoneLoadStart();
    void OnZoneLoadEnd();

    // ES: Tick por frame (lo llama SyncOrchestrator): timeouts, asentamiento de zona y callback.
    // EN:
    // ── Per-frame tick (called from SyncOrchestrator) ──
    void Tick();

    // ES: Diagnóstico.
    // EN: Diagnostics.
    // ── Diagnostics ──
    size_t GetPendingResourceCount() const;
    size_t GetLoadedResourceCount() const;
    int    GetBurstCount() const { return m_burstCount.load(std::memory_order_relaxed); }
    bool   IsGameLoaded() const { return m_gameLoaded.load(std::memory_order_relaxed); }
    bool   IsInBurst() const { return m_inBurst.load(std::memory_order_relaxed); }

    // ES: Motivo legible por el que IsSafeToSpawn() da false, o "OK" si es seguro.
    // EN:
    // Returns human-readable reason why IsSafeToSpawn() returns false, or "OK" if safe.
    std::string GetSpawnBlockReason() const;

    // ES: Callback que recibe cada tick si es seguro spawnear.
    // EN: Callback that receives every tick whether spawning is safe.
    // ── Spawn gate callback ──
    using SpawnGateCallback = std::function<void(bool safe)>;
    void SetSpawnGateCallback(SpawnGateCallback cb);

    // ES: Configuración de tiempos: enfriamiento tras carga (2 s), asentamiento de zona (1,5 s) y
    //     auto-limpieza de una ráfaga atascada (30 s).
    // EN: Timing configuration: cooldown after a load (2 s), zone settle time (1.5 s) and
    //     auto-clear of a stuck burst (30 s).
    // ── Configuration ──
    static constexpr int SPAWN_COOLDOWN_MS = 2000;  // Wait after last resource load
    static constexpr int ZONE_SETTLE_MS    = 1500;  // Wait after zone load completes
    static constexpr int BURST_TIMEOUT_MS  = 30000; // Auto-clear stuck burst after 30s

private:
    // ES: Cambia de fase, lo registra en el log y en el PipelineOrchestrator.
    // EN: Changes phase, logs it and records it in the PipelineOrchestrator.
    void TransitionTo(LoadingPhase newPhase);

    // ES: Fase actual; empieza en InitialLoad hasta que la partida termina de cargar.
    // EN: Current phase; starts at InitialLoad until the game finishes loading.
    std::atomic<LoadingPhase> m_phase{LoadingPhase::InitialLoad};

    // ES: Seguimiento de recursos (protegido por m_resourceMutex).
    // EN:
    // Resource tracking
    mutable std::mutex m_resourceMutex;
    std::unordered_map<std::string, ResourceState> m_resources;
    std::unordered_set<std::string> m_pendingResources;
    size_t m_totalLoaded = 0;

    // ES: Seguimiento de ráfagas de creación.
    // EN:
    // Burst tracking
    std::atomic<int>  m_burstCount{0};
    std::atomic<bool> m_gameLoaded{false};
    std::atomic<bool> m_inBurst{false};
    std::chrono::steady_clock::time_point m_burstStartTime{}; // For timeout auto-clear

    // ES: Marcas de tiempo (OJO: no son atómicas ni van bajo mutex en todos los accesos).
    // EN:
    // Timing
    std::chrono::steady_clock::time_point m_lastLoadCompleteTime;
    std::chrono::steady_clock::time_point m_zoneLoadEndTime;
    bool m_hasResourceHooks = false;  // True if Ogre hooks are installed

    // ES: Callback de la puerta de spawn y su mutex.
    // EN:
    // Callback
    SpawnGateCallback m_spawnGateCallback;
    mutable std::mutex m_callbackMutex;
};

} // namespace kmp
