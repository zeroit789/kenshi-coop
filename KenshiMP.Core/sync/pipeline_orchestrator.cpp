// ES: Implementación de PipelineOrchestrator (ver pipeline_orchestrator.h).
// EN: PipelineOrchestrator implementation (see pipeline_orchestrator.h).
#include "pipeline_orchestrator.h"
#include "../core.h"
#include "../hooks/entity_hooks.h"
#include "kmp/protocol.h"
#include "kmp/constants.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstdio>

namespace kmp {

// ES: CICLO DE VIDA
// ══════════════════════════════════════════════════════════════════════════════
// LIFECYCLE
// ══════════════════════════════════════════════════════════════════════════════

// ES: Guarda las referencias a los subsistemas y arranca los relojes.
// EN: Stores subsystem references and starts the clocks.
void PipelineOrchestrator::Initialize(PlayerID localId, EntityRegistry& registry,
                                       SpawnManager& spawnMgr, LoadingOrchestrator& loadingOrch,
                                       NetworkClient& client, NativeHud& hud) {
    m_localPlayerId = localId;
    m_registry      = &registry;
    m_spawnMgr      = &spawnMgr;
    m_loadingOrch   = &loadingOrch;
    m_client        = &client;
    m_hud           = &hud;

    auto now = std::chrono::steady_clock::now();
    m_connectionStart   = now;
    m_lastSnapshotTime  = now;
    m_lastAnomalyCheck  = now;
    m_gameLoadedTimeSet = false;
    m_initialized       = true;

    spdlog::info("PipelineOrchestrator: Initialized for player {}", localId);
}

// ES: Vacía snapshots remotos, eventos y anomalías.
// EN: Clears remote snapshots, events and anomalies.
void PipelineOrchestrator::Shutdown() {
    if (!m_initialized) return;
    m_initialized = false;

    {
        std::lock_guard lock(m_remoteMutex);
        m_remoteSnapshots.clear();
        m_remoteSnapshotTimes.clear();
    }
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_pendingBroadcast.clear();
    }
    {
        std::lock_guard lock(m_anomalyMutex);
        m_anomalies.clear();
    }

    spdlog::info("PipelineOrchestrator: Shutdown");
}

// ES: TEMPORIZACIÓN
// ══════════════════════════════════════════════════════════════════════════════
// TIMING
// ══════════════════════════════════════════════════════════════════════════════

// ES: Milisegundos desde Initialize (se desborda a los ~49 días, uint32).
// EN: Milliseconds since Initialize (wraps after ~49 days, uint32).
uint32_t PipelineOrchestrator::GetTimestampMs() const {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_connectionStart).count());
}

// ES: TICK
// ══════════════════════════════════════════════════════════════════════════════
// TICK
// ══════════════════════════════════════════════════════════════════════════════

// ES: Envoltorio SEH: TickInner usa objetos C++ (lock_guard, etc.) que impiden usar
//     __try directamente en ella. Este envoltorio fino captura cualquier violación de
//     acceso por punteros basura del juego (habitual en Steam, donde algunos
//     singletons no se resuelven).
// SEH wrapper — PipelineOrchestrator::TickInner has C++ objects (lock_guard, etc.)
// that prevent __try from being used directly in it. This thin wrapper catches any AV
// from garbage game pointers (common on Steam where singletons don't resolve).
static bool SEH_PipelineOrchestratorTick(PipelineOrchestrator* self) {
    __try {
        self->TickInner();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Ejecuta TickInner protegido; registra como mucho 5 fallos.
// EN: Runs TickInner protected; logs at most 5 crashes.
void PipelineOrchestrator::Tick(float /*deltaTime*/) {
    if (!m_initialized) return;

    if (!SEH_PipelineOrchestratorTick(this)) {
        static int s_tickCrashCount = 0;
        if (++s_tickCrashCount <= 5) {
            spdlog::error("PipelineOrchestrator::Tick SEH crash #{}", s_tickCrashCount);
        }
    }
}

// ES: Trabajo real del tick: snapshot a 1 Hz, envío de eventos, detección de anomalías
//     cada 2 s y refresco del HUD si está visible.
// EN: Actual tick work: 1 Hz snapshot, event sending, anomaly detection every 2 s and
//     HUD refresh when visible.
void PipelineOrchestrator::TickInner() {
    auto now = std::chrono::steady_clock::now();

    // ES: Anotar cuándo cargó el juego (para detectar CanSpawn atascado).
    // Track game loaded time for CanSpawn stuck detection
    if (!m_gameLoadedTimeSet && Core::Get().IsGameLoaded()) {
        m_gameLoadedTime = now;
        m_gameLoadedTimeSet = true;
    }

    // ES: Recogida y envío del snapshot a 1 Hz.
    // 1Hz snapshot collection + broadcast
    auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastSnapshotTime);
    if (sinceLast.count() >= SNAPSHOT_INTERVAL_MS) {
        PipelineSnapshot snap = CollectSnapshot();
        {
            std::lock_guard lock(m_snapshotMutex);
            m_lastLocalSnapshot = snap;
        }
        if (Core::Get().IsConnected()) {
            BroadcastSnapshot(snap);
        }
        m_lastSnapshotTime = now;
    }

    // ES: Enviar los eventos pendientes.
    // Broadcast pending events
    if (Core::Get().IsConnected()) {
        BroadcastPendingEvents();
    }

    // ES: Detección de anomalías (cada 2 s).
    // Anomaly detection (every 2s)
    auto sinceAnomaly = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastAnomalyCheck);
    if (sinceAnomaly.count() >= ANOMALY_CHECK_INTERVAL_MS) {
        RunAnomalyDetection();
        m_lastAnomalyCheck = now;
    }

    // ES: Refrescar el HUD si está visible.
    // Update HUD if visible
    if (m_hudVisible.load(std::memory_order_relaxed)) {
        UpdateHud();
    }
}

// ES: RECOGIDA DEL SNAPSHOT
// ══════════════════════════════════════════════════════════════════════════════
// SNAPSHOT COLLECTION
// ══════════════════════════════════════════════════════════════════════════════

// ES: Rellena un PipelineSnapshot con el estado actual de cada subsistema (contadores
//     truncados a 8/16 bits y flags por bits).
// EN: Fills a PipelineSnapshot with the current state of each subsystem (counters
//     truncated to 8/16 bits and bit flags).
PipelineSnapshot PipelineOrchestrator::CollectSnapshot() const {
    PipelineSnapshot snap{};
    auto& core = Core::Get();

    snap.version   = 1;
    snap.playerId  = m_localPlayerId;
    snap.snapshotTimestamp = GetTimestampMs();

    // ES: Estado de entity_hooks (creaciones/destrucciones de personajes y spawns in situ).
    // entity_hooks state
    snap.totalCreates      = static_cast<uint16_t>(entity_hooks::GetTotalCreates() & 0xFFFF);
    snap.totalDestroys     = static_cast<uint16_t>(entity_hooks::GetTotalDestroys() & 0xFFFF);
    snap.burstCount        = 0; // burst count within current window
    snap.inPlaceSpawnCount = static_cast<uint8_t>(
        std::min(entity_hooks::GetInPlaceSpawnCount(), 255));

    // ES: Flags de hooks: bit 0 = hook CharacterCreate activo, bit 1 = CharacterDestroy.
    //     Los bits 2 (inBurst) y 3 (loadingComplete) ya no se rellenan.
    snap.hookFlags = 0;
    auto hookDiags = HookManager::Get().GetDiagnostics();
    for (auto& d : hookDiags) {
        if (d.name == "CharacterCreate" && d.enabled) snap.hookFlags |= 0x01;
        if (d.name == "CharacterDestroy" && d.enabled) snap.hookFlags |= 0x02;
    }
    // ES: Detección de ráfagas eliminada: el hook se desactiva por completo durante la carga.
    // Burst detection removed — hook is disabled during loading entirely

    // ES: Estado de SpawnManager (spawns pendientes, plantillas, flags de la factoría).
    // SpawnManager state
    if (m_spawnMgr) {
        snap.pendingSpawnCount = static_cast<uint16_t>(
            std::min(m_spawnMgr->GetPendingSpawnCount(), (size_t)65535));
        snap.templateCount     = static_cast<uint16_t>(
            std::min(m_spawnMgr->GetTemplateCount(), (size_t)65535));
        snap.charTemplateCount = static_cast<uint8_t>(
            std::min(m_spawnMgr->GetCharacterTemplateCount(), (size_t)255));

        snap.factoryFlags = 0;
        if (m_spawnMgr->IsReady())          snap.factoryFlags |= 0x01;
        if (m_spawnMgr->HasPreCallData())   snap.factoryFlags |= 0x02;
        if (m_spawnMgr->HasRequestStruct()) snap.factoryFlags |= 0x04;
    }

    // ES: Estado de LoadingOrchestrator (fase, recursos pendientes, flags).
    // LoadingOrchestrator state
    if (m_loadingOrch) {
        snap.loadingPhase     = static_cast<uint8_t>(m_loadingOrch->GetPhase());
        snap.pendingResources = static_cast<uint8_t>(
            std::min(m_loadingOrch->GetPendingResourceCount(), (size_t)255));
        snap.orchFlags = 0;
        if (m_loadingOrch->IsGameLoaded())   snap.orchFlags |= 0x01;
        if (m_loadingOrch->IsSafeToSpawn())  snap.orchFlags |= 0x02;
        if (m_loadingOrch->GetBurstCount() > 0) snap.orchFlags |= 0x04;
    }

    // ES: Estado de EntityRegistry: locales, remotas, remotas con objeto y "fantasmas"
    //     (remotas sin objeto del juego).
    // EntityRegistry state
    if (m_registry) {
        size_t total  = m_registry->GetEntityCount();
        size_t remote = m_registry->GetRemoteCount();
        size_t spawned = m_registry->GetSpawnedRemoteCount();
        snap.localEntityCount   = static_cast<uint16_t>(total > remote ? total - remote : 0);
        snap.remoteEntityCount  = static_cast<uint16_t>(remote);
        snap.spawnedRemoteCount = static_cast<uint16_t>(spawned);
        snap.ghostCount         = static_cast<uint16_t>(remote > spawned ? remote - spawned : 0);
    }

    // ES: Estado de Core (último paso, flags, ping, tiempo activo). El bit 4
    //     (pipelineStarted) no se rellena.
    // Core state
    snap.lastCompletedStep = static_cast<uint8_t>(core.GetLastCompletedStep());
    snap.coreFlags = 0;
    if (core.IsHost())       snap.coreFlags |= 0x01;
    if (core.IsLoading())    snap.coreFlags |= 0x02;
    if (core.IsGameLoaded()) snap.coreFlags |= 0x04;
    if (core.IsConnected())  snap.coreFlags |= 0x08;
    snap.ping = static_cast<uint16_t>(
        std::min(core.GetClient().GetPing(), (uint32_t)65535));
    snap.uptimeSeconds = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_connectionStart).count());

    // ES: Estado de SyncOrchestrator (contador de ticks y flag de activo; el bit 1 no se rellena).
    // SyncOrchestrator state
    auto* syncOrch = core.GetSyncOrchestrator();
    if (syncOrch) {
        snap.syncTickCount = static_cast<uint16_t>(syncOrch->GetTickCount() & 0xFFFF);
        snap.syncFlags = 0;
        if (syncOrch->IsActive()) snap.syncFlags |= 0x01;
    }

    return snap;
}

// ES: RED: ENVÍO
// ══════════════════════════════════════════════════════════════════════════════
// NETWORK: BROADCAST
// ══════════════════════════════════════════════════════════════════════════════

// ES: Envía el snapshot tal cual (48 bytes crudos) como C2S_PipelineSnapshot, fiable no ordenado.
// EN: Sends the snapshot as-is (48 raw bytes) as C2S_PipelineSnapshot, reliable unordered.
void PipelineOrchestrator::BroadcastSnapshot(const PipelineSnapshot& snap) {
    if (!m_client) return;

    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_PipelineSnapshot);
    writer.WriteRaw(&snap, sizeof(PipelineSnapshot));
    m_client->SendReliableUnordered(writer.Data(), writer.Size());
}

// ES: Envía un lote de hasta EVENT_BROADCAST_BATCH eventos pendientes como C2S_PipelineEvent.
// EN: Sends a batch of up to EVENT_BROADCAST_BATCH pending events as C2S_PipelineEvent.
void PipelineOrchestrator::BroadcastPendingEvents() {
    if (!m_client) return;

    // FIX deadlock ABBA (m_eventMutex <-> m_enetMutex):
    // Extraemos el lote de eventos pendientes BAJO m_eventMutex, SOLTAMOS el lock,
    // y solo ENTONCES enviamos por red. Enviar dentro del scope del lock permitía
    // que el hilo de juego tomara m_eventMutex -> m_enetMutex mientras el hilo de
    // red tomaba m_enetMutex -> m_eventMutex, congelando ambos hilos.
    // Mismo patrón que BroadcastSnapshot: copiar bajo lock, enviar fuera de él.
    // EN: ABBA deadlock fix (m_eventMutex <-> m_enetMutex): take the batch under
    //     m_eventMutex, RELEASE the lock, and only THEN send over the network. Sending
    //     inside the lock let the game thread take m_eventMutex -> m_enetMutex while the
    //     network thread took m_enetMutex -> m_eventMutex, freezing both threads.
    //     Same pattern as BroadcastSnapshot: copy under lock, send outside it.
    std::vector<PipelineEvent> batch;
    {
        std::lock_guard lock(m_eventMutex);
        if (m_pendingBroadcast.empty()) return;

        // ES: Agrupar hasta EVENT_BROADCAST_BATCH eventos por paquete.
        // Batch up to EVENT_BROADCAST_BATCH events per packet
        int count = static_cast<int>(std::min(m_pendingBroadcast.size(),
                                               (size_t)EVENT_BROADCAST_BATCH));
        batch.reserve(count);
        for (int i = 0; i < count; i++) {
            batch.push_back(std::move(m_pendingBroadcast.front()));
            m_pendingBroadcast.pop_front();
        }
    } // m_eventMutex liberado ANTES de tocar la red

    // Serialización idéntica a la anterior, ahora usando el lote copiado 'batch'.
    // EN: Same serialization as before, now using the copied 'batch'.
    //     Per event: type, severity, padding, timestamp, entityId, auxData, detail (max 64).
    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_PipelineEvent);
    writer.WriteU8(static_cast<uint8_t>(batch.size()));

    for (const auto& evt : batch) {
        writer.WriteU8(static_cast<uint8_t>(evt.type));
        writer.WriteU8(evt.severity);
        writer.WriteU16(0); // padding
        writer.WriteU32(evt.timestamp);
        writer.WriteU32(evt.entityId);
        writer.WriteU32(evt.auxData);
        std::string detail = evt.detail.substr(0, 64);
        writer.WriteString(detail);
    }

    // Envío de red FUERA del lock -> ya no puede producirse el deadlock ABBA.
    // EN: Network send OUTSIDE the lock -> the ABBA deadlock can no longer happen.
    m_client->SendReliableUnordered(writer.Data(), writer.Size());
}

// ES: RED: RECEPCIÓN
// ══════════════════════════════════════════════════════════════════════════════
// NETWORK: RECEIVE
// ══════════════════════════════════════════════════════════════════════════════

// ES: Guarda el snapshot de otro jugador (descarta tamaños cortos y versiones != 1).
// EN: Stores another player's snapshot (drops short sizes and versions != 1).
void PipelineOrchestrator::OnRemoteSnapshot(PlayerID sender, const uint8_t* data, size_t size) {
    if (size < sizeof(PipelineSnapshot)) return;

    PipelineSnapshot snap{};
    std::memcpy(&snap, data, sizeof(PipelineSnapshot));

    // ES: Validar versión.
    // Validate version
    if (snap.version == 0 || snap.version > 1) return;

    {
        std::lock_guard lock(m_remoteMutex);
        m_remoteSnapshots[sender] = snap;
        m_remoteSnapshotTimes[sender] = std::chrono::steady_clock::now();
    }
}

// ES: Lee un lote de eventos de otro jugador (mismo formato que BroadcastPendingEvents)
//     y los añade al historial con el prefijo "[P<id>]".
// EN: Reads a batch of events from another player (same format as BroadcastPendingEvents)
//     and adds them to the history prefixed with "[P<id>]".
void PipelineOrchestrator::OnRemoteEvent(PlayerID sender, const uint8_t* data, size_t size) {
    PacketReader reader(data, size);

    uint8_t count;
    if (!reader.ReadU8(count)) return;
    if (count > EVENT_BROADCAST_BATCH) count = EVENT_BROADCAST_BATCH;

    std::lock_guard lock(m_eventMutex);
    for (int i = 0; i < count; i++) {
        PipelineEvent evt{};
        uint8_t typeRaw, sev;
        uint16_t pad;
        if (!reader.ReadU8(typeRaw)) break;
        if (!reader.ReadU8(sev)) break;
        if (!reader.ReadU16(pad)) break;
        if (!reader.ReadU32(evt.timestamp)) break;
        if (!reader.ReadU32(evt.entityId)) break;
        if (!reader.ReadU32(evt.auxData)) break;
        if (!reader.ReadString(evt.detail, 64)) break;

        evt.type = static_cast<PipelineEventType>(typeRaw);
        evt.severity = sev;

        // ES: Añadir al detalle quién lo envió.
        // Prefix detail with sender info
        evt.detail = "[P" + std::to_string(sender) + "] " + evt.detail;

        m_events.push_back(std::move(evt));
        if (m_events.size() > MAX_LOCAL_EVENTS) {
            m_events.pop_front();
        }
    }
}

// ES: REGISTRO DE EVENTOS
// ══════════════════════════════════════════════════════════════════════════════
// EVENT RECORDING
// ══════════════════════════════════════════════════════════════════════════════

// ES: Añade un evento local al historial (máx. MAX_LOCAL_EVENTS) y a la cola de envío.
// EN: Adds a local event to the history (max MAX_LOCAL_EVENTS) and to the send queue.
void PipelineOrchestrator::RecordEvent(PipelineEventType type, uint8_t severity,
                                        EntityID entityId, uint32_t auxData,
                                        const std::string& detail) {
    if (!m_initialized) return;

    PipelineEvent evt;
    evt.type      = type;
    evt.severity  = severity;
    evt.timestamp = GetTimestampMs();
    evt.entityId  = entityId;
    evt.auxData   = auxData;
    evt.detail    = detail;

    std::lock_guard lock(m_eventMutex);
    m_events.push_back(evt);
    if (m_events.size() > MAX_LOCAL_EVENTS) {
        m_events.pop_front();
    }
    m_pendingBroadcast.push_back(evt);
}

// ES: DETECCIÓN DE ANOMALÍAS
// ══════════════════════════════════════════════════════════════════════════════
// ANOMALY DETECTION
// ══════════════════════════════════════════════════════════════════════════════

// ES: Compara el snapshot local con los remotos y levanta/resuelve anomalías.
// EN: Compares the local snapshot with the remote ones and raises/resolves anomalies.
void PipelineOrchestrator::RunAnomalyDetection() {
    auto now = std::chrono::steady_clock::now();
    PipelineSnapshot local;
    {
        std::lock_guard lock(m_snapshotMutex);
        local = m_lastLocalSnapshot;
    }

    std::lock_guard remoteLock(m_remoteMutex);

    for (auto& [peerId, remoteSnap] : m_remoteSnapshots) {
        // ES: El otro tiene >5 spawns pendientes y nosotros 0.
        // Spawn queue mismatch
        if (remoteSnap.pendingSpawnCount > 5 && local.pendingSpawnCount == 0) {
            RaiseAnomaly(AnomalyType::SpawnQueueMismatch, peerId,
                "P" + std::to_string(peerId) + " has " +
                std::to_string(remoteSnap.pendingSpawnCount) +
                " pending spawns, local has 0");
        } else {
            ResolveAnomaly(AnomalyType::SpawnQueueMismatch, peerId);
        }

        // ES: Tenemos >3 entidades fantasma (remotas sin objeto) y el otro sí tiene spawns.
        // Entity ghost mismatch
        if (remoteSnap.spawnedRemoteCount > 0 && local.ghostCount > 3) {
            RaiseAnomaly(AnomalyType::EntityGhostMismatch, m_localPlayerId,
                "Local has " + std::to_string(local.ghostCount) +
                " ghost entities (no game object)");
        } else {
            ResolveAnomaly(AnomalyType::EntityGhostMismatch, m_localPlayerId);
        }

        // ES: El snapshot del otro está caducado (>5 s).
        // Snapshot staleness
        auto& snapTime = m_remoteSnapshotTimes[peerId];
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - snapTime);
        if (elapsed.count() > SNAPSHOT_STALE_THRESHOLD_MS) {
            RaiseAnomaly(AnomalyType::SnapshotStale, peerId,
                "No snapshot from P" + std::to_string(peerId) +
                " for " + std::to_string(elapsed.count() / 1000) + "s");
        } else {
            ResolveAnomaly(AnomalyType::SnapshotStale, peerId);
        }
    }

    // ES: CanSpawn sigue a false más de 30 s después de cargar el juego (local).
    // CanSpawn stuck (local)
    if (m_gameLoadedTimeSet) {
        auto sinceLoad = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_gameLoadedTime);
        bool canSpawn = (local.orchFlags & 0x02) != 0;
        if (!canSpawn && sinceLoad.count() > CANSPAWN_STUCK_THRESHOLD_MS) {
            RaiseAnomaly(AnomalyType::CanSpawnStuck, m_localPlayerId,
                "CanSpawn stuck false for " + std::to_string(sinceLoad.count() / 1000) +
                "s (phase=" + std::string(GetPhaseName(local.loadingPhase)) + ")");
        } else if (canSpawn) {
            ResolveAnomaly(AnomalyType::CanSpawnStuck, m_localPlayerId);
        }
    }

    // ES: El hook CharacterCreate no ha saltado ni una vez tras 30 s.
    // Hook not firing
    if (local.uptimeSeconds > 30 && local.totalCreates == 0) {
        RaiseAnomaly(AnomalyType::HookNotFiring, m_localPlayerId,
            "CharacterCreate: 0 calls after " + std::to_string(local.uptimeSeconds) + "s");
    } else if (local.totalCreates > 0) {
        ResolveAnomaly(AnomalyType::HookNotFiring, m_localPlayerId);
    }

    // ES: Fase de carga distinta de Idle con más de 60 s de tiempo activo. Ojo: mide el
    //     tiempo desde la conexión, no el tiempo en esa fase, así que el texto
    //     "stuck for >60s" puede ser engañoso.
    // Phase stuck
    if (local.loadingPhase != 0 && local.uptimeSeconds > 60) {
        RaiseAnomaly(AnomalyType::PhaseStuck, m_localPlayerId,
            "Loading phase " + std::string(GetPhaseName(local.loadingPhase)) +
            " stuck for >60s");
    } else if (local.loadingPhase == 0) {
        ResolveAnomaly(AnomalyType::PhaseStuck, m_localPlayerId);
    }
}

// ES: Añade una anomalía si no hay ya una activa igual (tipo + jugador); guarda como
//     mucho MAX_ANOMALIES y la avisa en el log y en el HUD.
// EN: Adds an anomaly unless an identical active one exists (type + player); keeps at
//     most MAX_ANOMALIES and reports it in the log and the HUD.
void PipelineOrchestrator::RaiseAnomaly(AnomalyType type, PlayerID source,
                                         const std::string& desc) {
    std::lock_guard lock(m_anomalyMutex);

    // ES: ¿Ya está levantada (sin resolver)?
    // Check if already raised (not resolved)
    for (auto& a : m_anomalies) {
        if (a.type == type && a.sourcePlayer == source && !a.resolved) {
            return; // Already active
        }
    }

    PipelineAnomaly anomaly;
    anomaly.type         = type;
    anomaly.sourcePlayer = source;
    anomaly.detectedAt   = GetTimestampMs();
    anomaly.description  = desc;
    anomaly.resolved     = false;

    m_anomalies.push_back(std::move(anomaly));
    if (m_anomalies.size() > MAX_ANOMALIES) {
        m_anomalies.erase(m_anomalies.begin());
    }

    spdlog::warn("PipelineOrchestrator: ANOMALY — {}", desc);
    if (m_hud) {
        m_hud->AddSystemMessage("[Pipeline] " + desc);
    }
}

// ES: Marca como resueltas las anomalías activas de ese tipo y jugador.
// EN: Marks active anomalies of that type and player as resolved.
void PipelineOrchestrator::ResolveAnomaly(AnomalyType type, PlayerID source) {
    std::lock_guard lock(m_anomalyMutex);
    for (auto& a : m_anomalies) {
        if (a.type == type && a.sourcePlayer == source && !a.resolved) {
            a.resolved = true;
        }
    }
}

// ES: HUD
// ══════════════════════════════════════════════════════════════════════════════
// HUD
// ══════════════════════════════════════════════════════════════════════════════

// ES: Alterna la visibilidad del HUD de depuración.
// EN: Toggles the debug HUD visibility.
void PipelineOrchestrator::ToggleHud() {
    bool current = m_hudVisible.load(std::memory_order_relaxed);
    m_hudVisible.store(!current, std::memory_order_relaxed);
}

// ES: Escribe en el HUD las líneas de estado local, de cada jugador remoto y las
//     anomalías activas. "Burst" siempre sale N (el bit ya no se rellena).
// EN: Writes local status lines, one per remote player and the active anomalies to
//     the HUD. "Burst" always shows N (the bit is no longer set).
void PipelineOrchestrator::UpdateHud() {
    if (!m_hud) return;

    PipelineSnapshot local;
    {
        std::lock_guard lock(m_snapshotMutex);
        local = m_lastLocalSnapshot;
    }

    char buf[256];

    snprintf(buf, sizeof(buf), "Phase:%s CanSpawn:%s Burst:%s Step:%d",
             GetPhaseName(local.loadingPhase),
             (local.orchFlags & 0x02) ? "Y" : "N",
             (local.hookFlags & 0x04) ? "Y" : "N",
             local.lastCompletedStep);
    m_hud->LogStep("PIPE", buf);

    snprintf(buf, sizeof(buf), "Creates:%d Destroys:%d InPlace:%d Pending:%d",
             local.totalCreates, local.totalDestroys,
             local.inPlaceSpawnCount, local.pendingSpawnCount);
    m_hud->LogStep("PIPE", buf);

    snprintf(buf, sizeof(buf), "Local:%d Remote:%d Ghost:%d Tpl:%d/%d",
             local.localEntityCount, local.remoteEntityCount,
             local.ghostCount, local.charTemplateCount, local.templateCount);
    m_hud->LogStep("PIPE", buf);

    // ES: Resumen de cada jugador remoto.
    // Remote peer summaries
    {
        std::lock_guard remoteLock(m_remoteMutex);
        for (auto& [peerId, snap] : m_remoteSnapshots) {
            snprintf(buf, sizeof(buf), "P%u: Phase:%s Spawn:%s Ent:%d/%d Ghost:%d Ping:%d",
                     peerId, GetPhaseName(snap.loadingPhase),
                     (snap.orchFlags & 0x02) ? "Y" : "N",
                     snap.localEntityCount, snap.remoteEntityCount,
                     snap.ghostCount, snap.ping);
            m_hud->LogStep("PEER", buf);
        }
    }

    // ES: Anomalías activas.
    // Active anomalies
    {
        std::lock_guard anomalyLock(m_anomalyMutex);
        for (auto& a : m_anomalies) {
            if (!a.resolved) {
                m_hud->LogStep("WARN", a.description);
            }
        }
    }
}

// ES: CONSULTAS
// ══════════════════════════════════════════════════════════════════════════════
// QUERIES
// ══════════════════════════════════════════════════════════════════════════════

// ES: Copia del último snapshot local.
// EN: Copy of the last local snapshot.
PipelineSnapshot PipelineOrchestrator::GetLocalSnapshot() const {
    std::lock_guard lock(m_snapshotMutex);
    return m_lastLocalSnapshot;
}

// ES: Copia de los snapshots remotos.
// EN: Copy of the remote snapshots.
std::unordered_map<PlayerID, PipelineSnapshot> PipelineOrchestrator::GetRemoteSnapshots() const {
    std::lock_guard lock(m_remoteMutex);
    return m_remoteSnapshots;
}

// ES: Los últimos maxCount eventos, en orden cronológico.
// EN: The last maxCount events, in chronological order.
std::vector<PipelineEvent> PipelineOrchestrator::GetRecentEvents(int maxCount) const {
    std::lock_guard lock(m_eventMutex);
    std::vector<PipelineEvent> result;
    int start = static_cast<int>(m_events.size()) - maxCount;
    if (start < 0) start = 0;
    for (int i = start; i < static_cast<int>(m_events.size()); i++) {
        result.push_back(m_events[i]);
    }
    return result;
}

// ES: Copia de las anomalías sin resolver.
// EN: Copy of the unresolved anomalies.
std::vector<PipelineAnomaly> PipelineOrchestrator::GetActiveAnomalies() const {
    std::lock_guard lock(m_anomalyMutex);
    std::vector<PipelineAnomaly> result;
    for (auto& a : m_anomalies) {
        if (!a.resolved) result.push_back(a);
    }
    return result;
}

// ES: Últimos maxCount eventos de una entidad, en orden cronológico.
// EN: Last maxCount events of one entity, in chronological order.
std::vector<PipelineEvent> PipelineOrchestrator::GetEntityHistory(EntityID entityId, int maxCount) const {
    std::lock_guard lock(m_eventMutex);
    std::vector<PipelineEvent> result;
    for (auto it = m_events.rbegin(); it != m_events.rend() && (int)result.size() < maxCount; ++it) {
        if (it->entityId == entityId) {
            result.push_back(*it);
        }
    }
    std::reverse(result.begin(), result.end());
    return result;
}

// ES: FORMATO DE TEXTO
// ══════════════════════════════════════════════════════════════════════════════
// TEXT FORMATTING
// ══════════════════════════════════════════════════════════════════════════════

// ES: Nombre corto de un tipo de evento.
// EN: Short name of an event type.
static const char* EventTypeName(PipelineEventType type) {
    switch (type) {
    case PipelineEventType::SpawnQueued:       return "SpawnQueued";
    case PipelineEventType::SpawnSucceeded:    return "SpawnOK";
    case PipelineEventType::SpawnFailed:       return "SpawnFail";
    case PipelineEventType::SpawnRetried:      return "SpawnRetry";
    case PipelineEventType::SpawnCapReached:   return "SpawnCap";
    case PipelineEventType::BurstDetected:     return "BurstStart";
    case PipelineEventType::BurstEnded:        return "BurstEnd";
    case PipelineEventType::LoadingComplete:   return "LoadDone";
    case PipelineEventType::PhaseChanged:      return "PhaseChg";
    case PipelineEventType::EntityRegistered:  return "EntReg";
    case PipelineEventType::EntityLinked:      return "EntLink";
    case PipelineEventType::EntityUnregistered:return "EntUnreg";
    case PipelineEventType::HookInstalled:     return "HookInst";
    case PipelineEventType::HookCrashed:       return "HookCrash";
    case PipelineEventType::ConnectionLost:    return "ConnLost";
    case PipelineEventType::SnapshotReceived:  return "SnapRecv";
    default: return "???";
    }
}

// ES: Añade a 'out' un bloque de texto legible con el contenido de un snapshot.
// EN: Appends to 'out' a human-readable text block with a snapshot's contents.
static void FormatSnapshotBlock(const PipelineSnapshot& snap, const char* label,
                                 std::string& out) {
    char buf[512];

    snprintf(buf, sizeof(buf),
        "  --- %s (Player %u) ---\n"
        "  Phase: %s | CanSpawn: %s | GameLoaded: %s | Burst: %s\n"
        "  Creates: %u | Destroys: %u | InPlace: %u\n"
        "  Pending: %u | Templates: %u/%u | Factory: %s PreCall: %s\n"
        "  Entities — Local: %u Remote: %u Spawned: %u Ghost: %u\n"
        "  Core — Host: %s Loading: %s Step: %d Ping: %ums Uptime: %us\n"
        "  Hooks — Create: %s Destroy: %s\n",
        label, snap.playerId,
        GetPhaseName(snap.loadingPhase),
        (snap.orchFlags & 0x02) ? "YES" : "NO",
        (snap.orchFlags & 0x01) ? "YES" : "NO",
        (snap.hookFlags & 0x04) ? "YES" : "NO",
        snap.totalCreates, snap.totalDestroys, snap.inPlaceSpawnCount,
        snap.pendingSpawnCount, snap.charTemplateCount, snap.templateCount,
        (snap.factoryFlags & 0x01) ? "YES" : "NO",
        (snap.factoryFlags & 0x02) ? "YES" : "NO",
        snap.localEntityCount, snap.remoteEntityCount,
        snap.spawnedRemoteCount, snap.ghostCount,
        (snap.coreFlags & 0x01) ? "YES" : "NO",
        (snap.coreFlags & 0x02) ? "YES" : "NO",
        snap.lastCompletedStep, snap.ping, snap.uptimeSeconds,
        (snap.hookFlags & 0x01) ? "OK" : "OFF",
        (snap.hookFlags & 0x02) ? "OK" : "OFF");

    out += buf;
}

// ES: Volcado completo para /pipeline: snapshot local, remotos, anomalías y 10 últimos eventos.
// EN: Full dump for /pipeline: local snapshot, remotes, anomalies and last 10 events.
std::string PipelineOrchestrator::FormatStatusDump() const {
    std::string result;
    result.reserve(2048);
    result += "=== Pipeline Status ===\n";

    // ES: Snapshot local.
    // Local snapshot
    {
        std::lock_guard lock(m_snapshotMutex);
        FormatSnapshotBlock(m_lastLocalSnapshot, "LOCAL", result);
    }

    // ES: Snapshots remotos.
    // Remote snapshots
    {
        std::lock_guard lock(m_remoteMutex);
        for (auto& [peerId, snap] : m_remoteSnapshots) {
            std::string label = "REMOTE P" + std::to_string(peerId);
            FormatSnapshotBlock(snap, label.c_str(), result);
        }
    }

    // ES: Anomalías activas.
    // Active anomalies
    auto anomalies = GetActiveAnomalies();
    if (!anomalies.empty()) {
        result += "\n  --- Anomalies ---\n";
        for (auto& a : anomalies) {
            result += "  [!] " + a.description + "\n";
        }
    }

    // ES: Eventos recientes (los 10 últimos).
    // Recent events (last 10)
    auto events = GetRecentEvents(10);
    if (!events.empty()) {
        result += "\n  --- Recent Events ---\n";
        for (auto& e : events) {
            char buf[256];
            snprintf(buf, sizeof(buf), "  [%ums] %s ent=%u: %s\n",
                     e.timestamp, EventTypeName(e.type), e.entityId, e.detail.c_str());
            result += buf;
        }
    }

    return result;
}

// ES: Historial de eventos de una entidad (últimos 20) en texto.
// EN: A single entity's event history (last 20) as text.
std::string PipelineOrchestrator::FormatEntityTrack(EntityID entityId) const {
    auto events = GetEntityHistory(entityId, 20);
    if (events.empty()) {
        return "No pipeline events for entity " + std::to_string(entityId);
    }

    std::string result = "=== Entity " + std::to_string(entityId) + " Pipeline History ===\n";
    for (auto& e : events) {
        char buf[256];
        snprintf(buf, sizeof(buf), "  [%ums] %s (sev=%d aux=%u): %s\n",
                 e.timestamp, EventTypeName(e.type), e.severity, e.auxData, e.detail.c_str());
        result += buf;
    }
    return result;
}

} // namespace kmp
