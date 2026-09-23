// ES: SyncOrchestrator: orquestador del pipeline de sincronización por frame. Posee
//     los motores EntityResolver, ZoneEngine y PlayerEngine, y en cada tick del juego
//     ejecuta en orden las etapas: actualizar zonas, intercambiar el doble buffer de
//     datos del frame, aplicar posiciones remotas interpoladas, leer y enviar
//     posiciones/equipo locales, procesar spawns pendientes, lanzar trabajo en segundo
//     plano y actualizar jugadores.
// EN: SyncOrchestrator: per-frame sync pipeline orchestrator. It owns the
//     EntityResolver, ZoneEngine and PlayerEngine engines, and on every game tick runs
//     these stages in order: update zones, swap the frame data double buffer, apply
//     interpolated remote positions, read and send local positions/equipment,
//     process pending spawns, kick background work and update players.
#pragma once
#include "entity_resolver.h"
#include "zone_engine.h"
#include "player_engine.h"
#include "interpolation.h"
#include "../sys/task_orchestrator.h"
#include "../sys/frame_data.h"
#include "../game/spawn_manager.h"
#include "../net/client.h"
#include "kmp/types.h"
#include "kmp/constants.h"
#include <chrono>
#include <atomic>
#include <memory>

namespace kmp {

// ES: Declaraciones adelantadas.
// Forward declarations
class EntityRegistry;
class PlayerController;

// ES: Prioridad de sincronización de una entidad según la distancia en zonas al jugador.
// Priority bucket for entity sync
enum class SyncPriority : uint8_t {
    Critical = 0,  // Same zone as any player (highest frequency)
    Normal   = 1,  // Adjacent zone (standard 20Hz)
    Low      = 2,  // Two zones away (10Hz)
    None     = 3,  // Out of interest range (no sync)
};

// ES: Orquestador de sincronización (ver cabecera del fichero).
// EN: Sync orchestrator (see file header).
class SyncOrchestrator {
public:
    // ES: Recibe referencias a los subsistemas de Core: registro de entidades, controlador
    //     de jugadores, interpolador, gestor de spawns, cliente de red y orquestador de tareas.
    // EN: Takes references to Core subsystems: entity registry, player controller,
    //     interpolator, spawn manager, network client and task orchestrator.
    SyncOrchestrator(EntityRegistry& registry,
                     PlayerController& playerCtrl,
                     Interpolation& interp,
                     SpawnManager& spawnMgr,
                     NetworkClient& client,
                     TaskOrchestrator& taskOrch);

    // ES: ---- Inicialización ---- Activa el orquestador con el id/nombre local; Shutdown lo para.
    // ---- Initialization ----
    void Initialize(PlayerID localId, const std::string& playerName);
    void Shutdown();

    // ES: ---- Tick por frame (se llama desde Core::OnGameTick) ----
    //     Devuelve true si se ha hecho trabajo de sincronización.
    // ---- Per-Frame Tick (called from Core::OnGameTick) ----
    // Returns true if sync work was performed.
    bool Tick(float deltaTime);

    // ES: ---- Accesores a los motores propios ----
    // ---- Accessors ----
    EntityResolver& GetResolver()       { return m_resolver; }
    ZoneEngine&     GetZoneEngine()     { return m_zoneEngine; }
    PlayerEngine&   GetPlayerEngine()   { return m_playerEngine; }

    const EntityResolver& GetResolver() const       { return m_resolver; }
    const ZoneEngine&     GetZoneEngine() const     { return m_zoneEngine; }
    const PlayerEngine&   GetPlayerEngine() const   { return m_playerEngine; }

    // ES: ---- Prioridades ---- Calcula la prioridad de una entidad y si toca
    //     sincronizarla en este tick.
    // ---- Priority Management ----
    SyncPriority ComputePriority(EntityID entityId) const;
    bool ShouldSyncThisTick(EntityID entityId) const;

    // ES: ---- Estado ---- Activo y número de ticks.
    // ---- State ----
    bool IsActive() const { return m_active; }
    uint64_t GetTickCount() const { return m_tickCount; }

    // ES: ---- Acceso al doble buffer (compatibilidad con Core) ----
    // ---- Double Buffer Access (for Core compatibility) ----
    FrameData& GetWriteBuffer() { return m_frameData[m_writeBuffer]; }
    FrameData& GetReadBuffer()  { return m_frameData[m_readBuffer]; }

    // ES: ---- Reinicio (al desconectar) ----
    // ---- Reset (on disconnect) ----
    void Reset();

private:
    // ES: ---- Etapas del pipeline (en el orden en que se ejecutan en Tick) ----
    // ---- Pipeline Stages ----
    void StageUpdateZones();
    void StageSwapBuffers();
    void StageApplyRemotePositions();
    void StagePollAndSendPositions();
    void StagePollAndSendEquipment();
    void StageProcessSpawns();
    void StageKickBackgroundWork();
    void StageUpdatePlayers(float deltaTime);

    // ES: ---- Trabajos en segundo plano (se lanzan en el pool de TaskOrchestrator) ----
    // ---- Background Workers ----
    void BackgroundReadEntities();
    void BackgroundInterpolate();

    // ES: ---- Referencias a subsistemas (no son propiedad de esta clase) ----
    // ---- Subsystem references ----
    EntityRegistry&   m_registry;
    PlayerController& m_playerController;
    Interpolation&    m_interpolation;
    SpawnManager&     m_spawnManager;
    NetworkClient&    m_client;
    TaskOrchestrator& m_taskOrchestrator;

    // ES: ---- Motores propios ----
    // ---- Owned engines ----
    EntityResolver    m_resolver;
    ZoneEngine        m_zoneEngine;
    PlayerEngine      m_playerEngine;

    // ES: ---- Datos de frame con doble buffer (uno se escribe en segundo plano, el otro se lee) ----
    // ---- Double-buffered frame data ----
    FrameData         m_frameData[2];
    int               m_writeBuffer = 0;
    int               m_readBuffer  = 1;
    bool              m_pipelineStarted = false;

    // ES: ---- Estado de sincronización ----
    // ---- Sync state ----
    uint64_t          m_tickCount = 0;
    bool              m_active = false;
    PlayerID          m_localPlayerId = INVALID_PLAYER;

    // ES: ---- Temporización (reconstrucción del índice de zonas cada 500 ms) ----
    // ---- Timing ----
    std::chrono::steady_clock::time_point m_lastZoneRebuild;
    static constexpr int ZONE_REBUILD_INTERVAL_MS = 500;

    // ES: ---- Estado de la cola de spawns (temporizadores, intentos directos, escaneo del heap, avisos) ----
    // ---- Spawn queue state ----
    std::chrono::steady_clock::time_point m_firstPendingTime;
    bool              m_hasPendingTimer = false;
    int               m_directSpawnAttempts = 0;
    bool              m_shownWaitingMsg = false;
    bool              m_shownTimeoutMsg = false;
    std::chrono::steady_clock::time_point m_lastDirectAttempt;
    bool              m_heapScanned = false;
    int               m_heapScanAttempts = 0;
    std::chrono::steady_clock::time_point m_lastHeapScan;
    std::chrono::steady_clock::time_point m_lastSpawnLog;

    // ES: ---- Limitador de sondeos (posiciones y equipo; equipo cada 2 s) ----
    // ---- Poll throttle ----
    std::chrono::steady_clock::time_point m_lastPollTime;
    std::chrono::steady_clock::time_point m_lastEquipmentPollTime;
    static constexpr int EQUIPMENT_POLL_INTERVAL_MS = 2000; // 2 seconds

    // ES: ---- Diagnóstico ----
    // ---- Diagnostics ----
    int               m_diagTickCount = 0;
    std::chrono::steady_clock::time_point m_lastDiagLog;
};

} // namespace kmp
