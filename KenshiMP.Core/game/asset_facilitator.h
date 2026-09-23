// ES: asset_facilitator.h - Fachada simplificada sobre LoadingOrchestrator. Los hooks y el
//     sistema de spawn le preguntan cosas de alto nivel ("¿se puede spawnear ahora?", "¿estamos
//     cargando?") sin tocar el orquestador directamente. Es una capa fina de delegación.
// EN: asset_facilitator.h - Simplified facade over LoadingOrchestrator. Hooks and the spawn
//     system ask it high-level questions ("can we spawn now?", "are we loading?") without
//     touching the orchestrator directly. It is a thin delegation layer.
#pragma once
#include "loading_orchestrator.h"
#include <string>

namespace kmp {

// ES: AssetFacilitator: fachada singleton para consultas de carga/assets (ver ejemplo abajo).
// EN:
// AssetFacilitator — simplified facade for asset/loading operations.
//
// Instead of hooks and spawn systems needing to interact with
// LoadingOrchestrator directly, they call the Facilitator for
// high-level queries like "can we spawn right now?"
//
// Example:
//   auto& fac = AssetFacilitator::Get();
//   if (fac.CanSpawn()) { /* proceed with spawn */ }
//   fac.PreloadTemplate("Greenlander");
//
// Thin delegation layer — no logic duplication.

class AssetFacilitator {
public:
    // ES: Devuelve la instancia única (singleton).
    // EN: Returns the single instance (singleton).
    static AssetFacilitator& Get();

    // ES: Enlaza/desenlaza el orquestador; llamar a Bind una vez tras construir LoadingOrchestrator.
    // EN:
    // Must be called once after LoadingOrchestrator is constructed
    void Bind(LoadingOrchestrator* orchestrator);
    void Unbind();
    bool IsBound() const { return m_orch != nullptr; }

    // ════════════════════════════════════════════════════════════════
    // Spawn Safety (used by entity_hooks and SyncOrchestrator)
    // ════════════════════════════════════════════════════════════════

    // ES: ¿Se puede spawnear un personaje ahora con seguridad? Mira fase de carga, recursos
    //     pendientes y enfriamiento. Si no hay orquestador enlazado devuelve true (degradación suave).
    // EN:
    // Can we safely spawn a character right now?
    // Checks: loading phase, pending resources, cooldown timer.
    // Returns true if no orchestrator is bound (graceful degradation).
    bool CanSpawn() const;

    // ES: Pide precargar los recursos de una plantilla de personaje (por adelantado).
    // EN:
    // Request preloading resources for a template (anticipatory)
    void PreloadTemplate(const std::string& templateName);

    // ════════════════════════════════════════════════════════════════
    // Loading Phase (used by entity_hooks burst detection)
    // ════════════════════════════════════════════════════════════════

    // ES: Consultas de fase: carga inicial de partida, transición de zona y fase actual.
    // EN: Phase queries: initial game load, zone transition and current phase.
    bool IsGameLoading() const;
    bool IsZoneLoading() const;
    LoadingPhase GetPhase() const;

    // ════════════════════════════════════════════════════════════════
    // Diagnostics
    // ════════════════════════════════════════════════════════════════

    // ES: Instantánea de diagnóstico del estado de carga (recursos, fase, si es seguro spawnear...).
    // EN: Diagnostic snapshot of the loading state (resources, phase, whether spawning is safe...).
    struct LoadingStats {
        size_t pendingResources = 0;
        size_t loadedResources  = 0;
        LoadingPhase currentPhase = LoadingPhase::Idle;
        bool safeToSpawn = true;
        bool gameLoaded  = false;
        int  burstCount  = 0;
    };

    LoadingStats GetStats() const;

private:
    AssetFacilitator() = default;
    // ES: Orquestador enlazado (nullptr = sin enlazar).
    // EN: Bound orchestrator (nullptr = not bound).
    LoadingOrchestrator* m_orch = nullptr;
};

} // namespace kmp
