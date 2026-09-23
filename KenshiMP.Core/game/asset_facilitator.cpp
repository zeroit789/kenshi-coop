// ES: asset_facilitator.cpp - Implementación de la fachada AssetFacilitator: cada método
//     delega en LoadingOrchestrator y devuelve un valor seguro si no hay orquestador.
// EN: asset_facilitator.cpp - AssetFacilitator facade implementation: every method delegates
//     to LoadingOrchestrator and returns a safe value when no orchestrator is bound.
#include "asset_facilitator.h"
#include <spdlog/spdlog.h>

namespace kmp {

// ES: Singleton de Meyers (se crea la primera vez que se pide).
// EN: Meyers singleton (created on first use).
AssetFacilitator& AssetFacilitator::Get() {
    static AssetFacilitator instance;
    return instance;
}

// ES: Guarda el orquestador al que se delegan las consultas.
// EN: Stores the orchestrator that queries are delegated to.
void AssetFacilitator::Bind(LoadingOrchestrator* orchestrator) {
    m_orch = orchestrator;
    spdlog::info("AssetFacilitator: Bound to LoadingOrchestrator");
}

// ES: Suelta el orquestador (las consultas pasan a devolver valores por defecto).
// EN: Releases the orchestrator (queries fall back to default values).
void AssetFacilitator::Unbind() {
    m_orch = nullptr;
    spdlog::info("AssetFacilitator: Unbound");
}

// ES: True si es seguro spawnear; sin orquestador se permite (degradación suave).
// EN: True when spawning is safe; allowed when no orchestrator is bound.
bool AssetFacilitator::CanSpawn() const {
    // Graceful degradation: if no orchestrator, allow spawns
    if (!m_orch) return true;
    return m_orch->IsSafeToSpawn();
}

// ES: Reenvía la petición de precarga al orquestador.
// EN: Forwards the preload request to the orchestrator.
void AssetFacilitator::PreloadTemplate(const std::string& templateName) {
    if (m_orch) {
        m_orch->RequestPreload(templateName);
    }
}

// ES: True durante la carga inicial de la partida (fase InitialLoad).
// EN: True during the initial game load (InitialLoad phase).
bool AssetFacilitator::IsGameLoading() const {
    if (!m_orch) return false;
    return m_orch->GetPhase() == LoadingPhase::InitialLoad;
}

// ES: True durante una transición de zona (fase ZoneTransition).
// EN: True during a zone transition (ZoneTransition phase).
bool AssetFacilitator::IsZoneLoading() const {
    if (!m_orch) return false;
    return m_orch->GetPhase() == LoadingPhase::ZoneTransition;
}

// ES: Fase de carga actual (Idle si no hay orquestador).
// EN: Current loading phase (Idle when no orchestrator).
LoadingPhase AssetFacilitator::GetPhase() const {
    if (!m_orch) return LoadingPhase::Idle;
    return m_orch->GetPhase();
}

// ES: Rellena una instantánea de diagnóstico con los datos del orquestador.
// EN: Fills a diagnostic snapshot with the orchestrator's data.
AssetFacilitator::LoadingStats AssetFacilitator::GetStats() const {
    LoadingStats stats;
    if (!m_orch) return stats;

    stats.pendingResources = m_orch->GetPendingResourceCount();
    stats.loadedResources  = m_orch->GetLoadedResourceCount();
    stats.currentPhase     = m_orch->GetPhase();
    stats.safeToSpawn      = m_orch->IsSafeToSpawn();
    stats.gameLoaded       = m_orch->IsGameLoaded();
    stats.burstCount       = m_orch->GetBurstCount();
    return stats;
}

} // namespace kmp
