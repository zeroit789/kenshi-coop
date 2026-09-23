// ES: loading_orchestrator.cpp - Implementación de LoadingOrchestrator: transiciones de fase
//     según eventos de carga/ráfagas, puerta de spawn con enfriamiento y tick periódico.
// EN: loading_orchestrator.cpp - LoadingOrchestrator implementation: phase transitions driven
//     by load/burst events, spawn gate with cooldown and periodic tick.
#include "loading_orchestrator.h"
#include "../core.h"
#include "../sync/pipeline_state.h"
#include <spdlog/spdlog.h>

namespace kmp {

// ES: True solo si: fase Idle, sin ráfaga activa, partida cargada, sin recursos pendientes
//     (si hay hooks de recursos) y enfriamiento de SPAWN_COOLDOWN_MS cumplido.
// EN: True only if: Idle phase, no active burst, game loaded, no pending resources
//     (when resource hooks exist) and the SPAWN_COOLDOWN_MS cooldown has elapsed.
bool LoadingOrchestrator::IsSafeToSpawn() const {
    // Must be in Idle phase
    if (GetPhase() != LoadingPhase::Idle) return false;

    // Must not be in a burst
    if (m_inBurst.load(std::memory_order_relaxed)) return false;

    // Must have completed game loading
    if (!m_gameLoaded.load(std::memory_order_relaxed)) return false;

    // If resource hooks are installed, check for pending resources
    if (m_hasResourceHooks) {
        std::lock_guard lock(m_resourceMutex);
        if (!m_pendingResources.empty()) return false;
    }

    // Check cooldown since last resource load
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastLoadCompleteTime);
    if (elapsed.count() < SPAWN_COOLDOWN_MS) return false;

    return true;
}

// ES: Mismas comprobaciones que IsSafeToSpawn pero devolviendo el motivo en texto (para logs/HUD).
// EN: Same checks as IsSafeToSpawn but returning the reason as text (for logs/HUD).
std::string LoadingOrchestrator::GetSpawnBlockReason() const {
    auto phase = GetPhase();
    if (phase != LoadingPhase::Idle) {
        const char* phaseName = "?";
        switch (phase) {
            case LoadingPhase::InitialLoad: phaseName = "InitialLoad"; break;
            case LoadingPhase::ZoneTransition: phaseName = "ZoneTransition"; break;
            case LoadingPhase::SpawnLoad: phaseName = "SpawnLoad"; break;
            default: break;
        }
        return std::string("Phase=") + phaseName + " (need Idle)";
    }
    if (m_inBurst.load(std::memory_order_relaxed))
        return "Burst in progress";
    if (!m_gameLoaded.load(std::memory_order_relaxed))
        return "Game not loaded (OnGameLoaded never fired)";
    if (m_hasResourceHooks) {
        std::lock_guard lock(m_resourceMutex);
        if (!m_pendingResources.empty())
            return "Pending resources: " + std::to_string(m_pendingResources.size());
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastLoadCompleteTime);
    if (elapsed.count() < SPAWN_COOLDOWN_MS)
        return "Cooldown: " + std::to_string(SPAWN_COOLDOWN_MS - elapsed.count()) + "ms remaining";
    return "OK";
}

// ES: Precarga anticipada: hoy solo registra en el log (no-op hasta tener hooks de Ogre).
// EN: Anticipatory preload: currently only logs (no-op until Ogre hooks exist).
void LoadingOrchestrator::RequestPreload(const std::string& templateName) {
    // Anticipatory preload — currently a no-op until Ogre hooks are installed.
    // When resource hooks are active, this will trigger MeshManager::prepare()
    // for the template's associated mesh to reduce spawn latency.
    spdlog::debug("LoadingOrchestrator: Preload requested for '{}'", templateName);
}

// ES: Marca un recurso como pendiente y activa el modo "hay hooks de recursos".
// EN: Marks a resource as pending and enables "resource hooks present" mode.
void LoadingOrchestrator::OnResourceRequested(const std::string& name, const std::string& type) {
    std::lock_guard lock(m_resourceMutex);
    m_pendingResources.insert(name);

    auto& state = m_resources[name];
    state.name = name;
    state.type = type;
    state.loaded = false;
    state.failed = false;
    state.requestTime = std::chrono::steady_clock::now();

    m_hasResourceHooks = true;
}

// ES: Quita el recurso de pendientes, lo marca cargado y reinicia el enfriamiento.
// EN: Removes the resource from pending, marks it loaded and restarts the cooldown.
void LoadingOrchestrator::OnResourceLoaded(const std::string& name, const std::string& type) {
    std::lock_guard lock(m_resourceMutex);
    m_pendingResources.erase(name);

    auto it = m_resources.find(name);
    if (it != m_resources.end()) {
        it->second.loaded = true;
        it->second.completedTime = std::chrono::steady_clock::now();
    }

    m_totalLoaded++;
    m_lastLoadCompleteTime = std::chrono::steady_clock::now();
}

// ES: Igual que cargado pero marcándolo como fallido (y avisando en el log).
// EN: Same as loaded but marking it as failed (with a log warning).
void LoadingOrchestrator::OnResourceFailed(const std::string& name, const std::string& type) {
    std::lock_guard lock(m_resourceMutex);
    m_pendingResources.erase(name);

    auto it = m_resources.find(name);
    if (it != m_resources.end()) {
        it->second.failed = true;
        it->second.completedTime = std::chrono::steady_clock::now();
    }

    spdlog::warn("LoadingOrchestrator: Resource failed: '{}' ({})", name, type);
    m_lastLoadCompleteTime = std::chrono::steady_clock::now();
}

// ES: Inicio de una ráfaga de creación de personajes: si ocurre en juego (Idle y ya cargado)
//     se interpreta como carga de zona y se pasa a ZoneTransition.
// EN: Start of a character creation burst: if it happens in-game (Idle and already loaded)
//     it is treated as a zone load and the phase moves to ZoneTransition.
void LoadingOrchestrator::OnBurstDetected(int createCount) {
    m_inBurst.store(true, std::memory_order_relaxed);
    m_burstStartTime = std::chrono::steady_clock::now();
    m_burstCount.fetch_add(1, std::memory_order_relaxed);

    if (GetPhase() == LoadingPhase::Idle) {
        // Burst during gameplay = zone transition loading
        if (m_gameLoaded.load(std::memory_order_relaxed)) {
            TransitionTo(LoadingPhase::ZoneTransition);
        }
    }

    spdlog::info("LoadingOrchestrator: Burst detected (creates={}, phase={})",
                 createCount, static_cast<int>(GetPhase()));
}

// ES: Fin de ráfaga: reinicia el enfriamiento y vuelve a Idle desde InitialLoad/ZoneTransition.
// EN: Burst end: restarts the cooldown and returns to Idle from InitialLoad/ZoneTransition.
void LoadingOrchestrator::OnBurstEnded(int totalCreates) {
    m_inBurst.store(false, std::memory_order_relaxed);
    m_lastLoadCompleteTime = std::chrono::steady_clock::now();

    // Transition back to Idle after burst settles
    auto phase = GetPhase();
    if (phase == LoadingPhase::InitialLoad || phase == LoadingPhase::ZoneTransition) {
        TransitionTo(LoadingPhase::Idle);
    }

    spdlog::info("LoadingOrchestrator: Burst ended (totalCreates={}, phase -> Idle)", totalCreates);
}

// ES: Partida cargada: marca m_gameLoaded y pasa de InitialLoad a Idle.
// EN: Game loaded: sets m_gameLoaded and moves from InitialLoad to Idle.
void LoadingOrchestrator::OnGameLoaded() {
    m_gameLoaded.store(true, std::memory_order_relaxed);
    m_lastLoadCompleteTime = std::chrono::steady_clock::now();

    if (GetPhase() == LoadingPhase::InitialLoad) {
        TransitionTo(LoadingPhase::Idle);
    }

    spdlog::info("LoadingOrchestrator: Game loaded — transitioning to Idle");
}

// ES: Empieza una carga de zona (solo cuenta si la partida ya estaba cargada).
// EN: A zone load starts (only counts once the game is loaded).
void LoadingOrchestrator::OnZoneLoadStart() {
    if (m_gameLoaded.load(std::memory_order_relaxed)) {
        TransitionTo(LoadingPhase::ZoneTransition);
    }
}

// ES: Termina la carga de zona; el paso a Idle lo hace Tick() tras ZONE_SETTLE_MS.
// EN: Zone load ends; Tick() moves to Idle after ZONE_SETTLE_MS.
void LoadingOrchestrator::OnZoneLoadEnd() {
    m_zoneLoadEndTime = std::chrono::steady_clock::now();
    m_lastLoadCompleteTime = m_zoneLoadEndTime;

    // Don't immediately go Idle — wait for the cooldown in Tick()
    spdlog::debug("LoadingOrchestrator: Zone load completed");
}

// ES: Tick periódico: limpia ráfagas atascadas (>30 s), cierra la transición de zona tras el
//     asentamiento y llama al callback con el estado de la puerta de spawn (en cada tick, no
//     solo cuando cambia, aunque el comentario de abajo diga "on state change").
// EN: Periodic tick: clears stuck bursts (>30 s), closes the zone transition after the settle
//     time and calls the callback with the spawn gate state (on every tick, not only on change,
//     even though the comment below says "on state change").
void LoadingOrchestrator::Tick() {
    auto phase = GetPhase();

    // Burst timeout: if m_inBurst has been true for >30s, auto-clear it.
    // This prevents IsSafeToSpawn() from being permanently stuck if
    // OnBurstEnded() is never called (e.g. due to a missed event).
    if (m_inBurst.load(std::memory_order_relaxed)) {
        auto burstElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_burstStartTime);
        if (burstElapsed.count() >= BURST_TIMEOUT_MS) {
            spdlog::warn("LoadingOrchestrator: Burst timeout ({}ms) — auto-clearing stuck burst",
                         burstElapsed.count());
            m_inBurst.store(false, std::memory_order_relaxed);
            m_lastLoadCompleteTime = std::chrono::steady_clock::now();
            if (phase == LoadingPhase::InitialLoad || phase == LoadingPhase::ZoneTransition) {
                TransitionTo(LoadingPhase::Idle);
            }
        }
    }

    // Zone transition settling
    if (phase == LoadingPhase::ZoneTransition && !m_inBurst.load(std::memory_order_relaxed)) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_zoneLoadEndTime);
        if (elapsed.count() >= ZONE_SETTLE_MS) {
            TransitionTo(LoadingPhase::Idle);
        }
    }

    // Notify callback on state change
    bool safe = IsSafeToSpawn();
    {
        std::lock_guard lock(m_callbackMutex);
        if (m_spawnGateCallback) {
            m_spawnGateCallback(safe);
        }
    }
}

// ES: Nº de recursos pendientes y total cargado (bajo mutex).
// EN: Pending resource count and total loaded (under mutex).
size_t LoadingOrchestrator::GetPendingResourceCount() const {
    std::lock_guard lock(m_resourceMutex);
    return m_pendingResources.size();
}

size_t LoadingOrchestrator::GetLoadedResourceCount() const {
    std::lock_guard lock(m_resourceMutex);
    return m_totalLoaded;
}

// ES: Registra el callback de la puerta de spawn.
// EN: Registers the spawn gate callback.
void LoadingOrchestrator::SetSpawnGateCallback(SpawnGateCallback cb) {
    std::lock_guard lock(m_callbackMutex);
    m_spawnGateCallback = std::move(cb);
}

// ES: Cambio atómico de fase; si cambia, se loguea y se registra un evento PhaseChanged.
// EN: Atomic phase change; if it changes, it is logged and a PhaseChanged event is recorded.
void LoadingOrchestrator::TransitionTo(LoadingPhase newPhase) {
    auto old = m_phase.exchange(newPhase, std::memory_order_release);
    if (old != newPhase) {
        spdlog::info("LoadingOrchestrator: Phase {} -> {}",
                     static_cast<int>(old), static_cast<int>(newPhase));
        Core::Get().GetPipelineOrch().RecordEvent(
            PipelineEventType::PhaseChanged, 0, 0, static_cast<uint32_t>(newPhase),
            std::string(GetPhaseName(static_cast<uint8_t>(old))) + " -> " +
            GetPhaseName(static_cast<uint8_t>(newPhase)));
    }
}

} // namespace kmp
