// ES: PipelineOrchestrator: depurador del pipeline replicado por red. Recoge el estado
//     local (hooks, spawns, carga, entidades, Core, sync) en un PipelineSnapshot de
//     48 bytes, lo envía cada segundo al servidor (que lo reenvía a los demás), recibe
//     los de otros jugadores, guarda un historial de eventos, detecta anomalías y
//     pinta un HUD de depuración. Lo consulta el comando /pipeline.
// EN: PipelineOrchestrator: network-replicated pipeline debugger. It collects local
//     state (hooks, spawns, loading, entities, Core, sync) into a 48-byte
//     PipelineSnapshot, sends it every second to the server (which relays it to the
//     others), receives other players' snapshots, keeps an event history, detects
//     anomalies and draws a debug HUD. The /pipeline command queries it.
#pragma once
#include "pipeline_state.h"
#include "kmp/types.h"
#include "kmp/protocol.h"
#include <mutex>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>

namespace kmp {

// ES: Declaraciones adelantadas.
// Forward declarations
class EntityRegistry;
class SpawnManager;
class LoadingOrchestrator;
class NetworkClient;
class NativeHud;

// ES: PipelineOrchestrator — depurador del pipeline replicado por red.
//     Recoge el estado local, intercambia snapshots con los demás jugadores,
//     detecta anomalías y dibuja un HUD de depuración.
// PipelineOrchestrator — network-replicated pipeline debugger.
// Collects local pipeline state, exchanges snapshots with peers,
// detects anomalies, and renders a debug HUD.
class PipelineOrchestrator {
public:
    // ES: Constructor por defecto; se configura en Initialize.
    // EN: Default constructor; configured in Initialize.
    PipelineOrchestrator() = default;

    // ES: ── Ciclo de vida ── Guarda referencias a los subsistemas / limpia el estado.
    // ── Lifecycle ──
    void Initialize(PlayerID localId, EntityRegistry& registry, SpawnManager& spawnMgr,
                    LoadingOrchestrator& loadingOrch, NetworkClient& client, NativeHud& hud);
    void Shutdown();

    // ES: ── Tick por frame (desde Core::OnGameTick, paso 11) ── Tick envuelve TickInner
    //     en un bloque SEH para sobrevivir a punteros basura del juego.
    // ── Per-frame tick (called from Core::OnGameTick, Step 11) ──
    void Tick(float deltaTime);
    void TickInner(); // Called from SEH wrapper in Tick()

    // ES: ── Registro de eventos (thread-safe, desde cualquier hilo) ──
    // ── Event Recording (thread-safe, called from any thread) ──
    void RecordEvent(PipelineEventType type, uint8_t severity,
                     EntityID entityId, uint32_t auxData,
                     const std::string& detail);

    // ES: ── Manejadores de red (los llama PacketHandler en el hilo de red) ──
    //     Snapshot remoto y lote de eventos remotos.
    // ── Network Handlers (called from PacketHandler on network thread) ──
    void OnRemoteSnapshot(PlayerID sender, const uint8_t* data, size_t size);
    void OnRemoteEvent(PlayerID sender, const uint8_t* data, size_t size);

    // ES: ── Mostrar/ocultar el HUD ──
    // ── HUD Toggle ──
    void ToggleHud();
    bool IsHudVisible() const { return m_hudVisible.load(std::memory_order_relaxed); }

    // ES: ── Consultas ── Snapshot local, snapshots remotos, eventos recientes, anomalías
    //     activas e historial de eventos de una entidad.
    // ── Queries ──
    PipelineSnapshot GetLocalSnapshot() const;
    std::unordered_map<PlayerID, PipelineSnapshot> GetRemoteSnapshots() const;
    std::vector<PipelineEvent> GetRecentEvents(int maxCount = 50) const;
    std::vector<PipelineAnomaly> GetActiveAnomalies() const;
    std::vector<PipelineEvent> GetEntityHistory(EntityID entityId, int maxCount = 20) const;

    // ES: ── Volcados de texto (para el comando /pipeline) ──
    // ── Text dumps (for /pipeline command) ──
    std::string FormatStatusDump() const;
    std::string FormatEntityTrack(EntityID entityId) const;

    // ES: ── Configuración ── Intervalos, tamaños de los buffers y umbrales de anomalías.
    //     Nota: HOOK_SILENT_THRESHOLD_MS y PHASE_STUCK_THRESHOLD_MS no se usan en el .cpp
    //     (allí están fijados 30 s y 60 s a mano).
    // EN: Note: HOOK_SILENT_THRESHOLD_MS and PHASE_STUCK_THRESHOLD_MS are not used in the
    //     .cpp (30 s and 60 s are hard-coded there).
    // ── Configuration ──
    static constexpr int SNAPSHOT_INTERVAL_MS       = 1000;
    static constexpr int EVENT_BROADCAST_BATCH      = 8;
    static constexpr int MAX_LOCAL_EVENTS           = 200;
    static constexpr int MAX_ANOMALIES              = 32;
    static constexpr int ANOMALY_CHECK_INTERVAL_MS  = 2000;
    static constexpr int CANSPAWN_STUCK_THRESHOLD_MS= 30000;
    static constexpr int HOOK_SILENT_THRESHOLD_MS   = 30000;
    static constexpr int PHASE_STUCK_THRESHOLD_MS   = 60000;
    static constexpr int SNAPSHOT_STALE_THRESHOLD_MS= 5000;

private:
    // ES: Funciones internas: recoger/enviar snapshot, enviar eventos pendientes,
    //     detección de anomalías (levantar/resolver), refresco del HUD y reloj en ms.
    // EN: Internal functions: collect/send snapshot, send pending events, anomaly
    //     detection (raise/resolve), HUD refresh and ms clock.
    PipelineSnapshot CollectSnapshot() const;
    void BroadcastSnapshot(const PipelineSnapshot& snap);
    void BroadcastPendingEvents();
    void RunAnomalyDetection();
    void RaiseAnomaly(AnomalyType type, PlayerID source, const std::string& desc);
    void ResolveAnomaly(AnomalyType type, PlayerID source);
    void UpdateHud();

    uint32_t GetTimestampMs() const;

    // ES: Referencias a subsistemas (punteros, se fijan en Initialize).
    // Subsystem references
    EntityRegistry*      m_registry    = nullptr;
    SpawnManager*        m_spawnMgr    = nullptr;
    LoadingOrchestrator* m_loadingOrch = nullptr;
    NetworkClient*       m_client      = nullptr;
    NativeHud*           m_hud         = nullptr;
    PlayerID             m_localPlayerId = 0;

    // ES: Temporización.
    // Timing
    std::chrono::steady_clock::time_point m_connectionStart;
    std::chrono::steady_clock::time_point m_lastSnapshotTime;
    std::chrono::steady_clock::time_point m_lastAnomalyCheck;
    std::chrono::steady_clock::time_point m_gameLoadedTime;
    bool m_gameLoadedTimeSet = false;

    // ES: Caché del último snapshot local.
    // Local snapshot cache
    mutable std::mutex m_snapshotMutex;
    PipelineSnapshot   m_lastLocalSnapshot{};

    // ES: Snapshots remotos y cuándo llegó el último de cada jugador.
    // Remote snapshots
    mutable std::mutex m_remoteMutex;
    std::unordered_map<PlayerID, PipelineSnapshot> m_remoteSnapshots;
    std::unordered_map<PlayerID, std::chrono::steady_clock::time_point> m_remoteSnapshotTimes;

    // ES: Buffers de eventos: todos (locales + remotos) y los pendientes de enviar.
    // Event ring buffers
    mutable std::mutex m_eventMutex;
    std::deque<PipelineEvent> m_events;             // All events (local + remote)
    std::deque<PipelineEvent> m_pendingBroadcast;   // Queued for network send

    // ES: Estado de anomalías.
    // Anomaly state
    mutable std::mutex m_anomalyMutex;
    std::vector<PipelineAnomaly> m_anomalies;

    // ES: Estado del HUD.
    // HUD state
    std::atomic<bool> m_hudVisible{false};
    bool m_initialized = false;
};

} // namespace kmp
