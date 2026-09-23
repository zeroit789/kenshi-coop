// ES: Tipos de datos del depurador de pipeline replicado por red: tipos de evento,
//     el registro PipelineEvent, el snapshot binario compacto PipelineSnapshot (48 bytes,
//     se envía a 1 Hz a los demás jugadores), tipos de anomalía y ayudas de formato.
// EN: Data types for the network-replicated pipeline debugger: event types, the
//     PipelineEvent record, the compact binary PipelineSnapshot (48 bytes, sent at 1 Hz
//     to the other players), anomaly types and formatting helpers.
#pragma once
#include "kmp/types.h"
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace kmp {

// ES: TIPOS DE EVENTO DEL PIPELINE (spawn, carga, entidades, hooks y red).
// ══════════════════════════════════════════════════════════════════════════════
// PIPELINE EVENT TYPES
// ══════════════════════════════════════════════════════════════════════════════

enum class PipelineEventType : uint8_t {
    // Spawn lifecycle
    SpawnQueued         = 0x01,
    SpawnSucceeded      = 0x02,
    SpawnFailed         = 0x03,
    SpawnRetried        = 0x04,
    SpawnCapReached     = 0x05,

    // Loading lifecycle
    BurstDetected       = 0x10,
    BurstEnded          = 0x11,
    LoadingComplete     = 0x12,
    PhaseChanged        = 0x13,

    // Entity lifecycle
    EntityRegistered    = 0x20,
    EntityLinked        = 0x21,
    EntityUnregistered  = 0x22,

    // Hook lifecycle
    HookInstalled       = 0x30,
    HookCrashed         = 0x32,

    // Network
    ConnectionLost      = 0x40,
    SnapshotReceived    = 0x41,
};

// ES: ── Evento del pipeline ──
//     Registro de longitud variable para el buffer circular y el envío por red.
//     Parte fija: 16 bytes. Texto de detalle: máximo 64 caracteres.
// ── Pipeline Event ──
// Variable-length record for ring buffer + network broadcast.
// Fixed part: 16 bytes. Detail string: max 64 chars.
struct PipelineEvent {
    PipelineEventType type;
    uint8_t           severity;     // 0=info, 1=warning, 2=error
    uint16_t          padding = 0;
    uint32_t          timestamp;    // Milliseconds since connection (wrapping)
    uint32_t          entityId;     // Related entity (0 if N/A)
    uint32_t          auxData;      // Type-specific value
    std::string       detail;       // Human-readable (max 64 chars on wire)
};

// ES: SNAPSHOT DEL PIPELINE — 48 bytes empaquetados (#pragma pack 1).
//     Snapshot binario compacto del estado local del pipeline, enviado a 1 Hz.
//     Todos los campos son de tamaño fijo para serializarlo tal cual con WriteRaw/ReadRaw.
//     Los comentarios "(N bytes)" de cada grupo suman 48; el static_assert lo comprueba.
// ══════════════════════════════════════════════════════════════════════════════
// PIPELINE SNAPSHOT — 48 bytes packed
// ══════════════════════════════════════════════════════════════════════════════
// Compact binary snapshot of local pipeline state sent at 1Hz.
// All fields are fixed-size for trivial serialization via WriteRaw/ReadRaw.

#pragma pack(push, 1)
struct PipelineSnapshot {
    // Identity (4 bytes)
    PlayerID  playerId;

    // entity_hooks state (8 bytes)
    uint16_t  totalCreates;
    uint16_t  totalDestroys;
    uint16_t  burstCount;
    uint8_t   hookFlags;          // Bit 0: createHookActive, Bit 1: destroyHookActive
                                  // Bit 2: inBurst, Bit 3: loadingComplete
    uint8_t   inPlaceSpawnCount;  // Truncated to 8-bit

    // SpawnManager state (6 bytes)
    uint16_t  pendingSpawnCount;
    uint16_t  templateCount;
    uint8_t   factoryFlags;       // Bit 0: hasFactory, Bit 1: hasPreCallData,
                                  // Bit 2: hasRequestStruct
    uint8_t   charTemplateCount;

    // LoadingOrchestrator state (4 bytes)
    uint8_t   loadingPhase;       // LoadingPhase enum cast
    uint8_t   pendingResources;
    uint8_t   orchFlags;          // Bit 0: gameLoaded, Bit 1: canSpawn, Bit 2: inBurst
    uint8_t   reserved1;

    // EntityRegistry state (8 bytes)
    uint16_t  localEntityCount;
    uint16_t  remoteEntityCount;
    uint16_t  spawnedRemoteCount;
    uint16_t  ghostCount;

    // Core state (8 bytes)
    uint8_t   lastCompletedStep;
    uint8_t   coreFlags;          // Bit 0: isHost, Bit 1: isLoading, Bit 2: gameLoaded,
                                  // Bit 3: connected, Bit 4: pipelineStarted
    uint16_t  ping;
    uint32_t  uptimeSeconds;

    // SyncOrchestrator state (4 bytes)
    uint16_t  syncTickCount;
    uint8_t   syncFlags;          // Bit 0: active, Bit 1: pipelineStarted
    uint8_t   reserved2;

    // Timestamp + version (6 bytes)
    uint32_t  snapshotTimestamp;  // steady_clock ms since connection start
    uint8_t   version;           // Snapshot format version (for forward compat)
    uint8_t   reserved3;
};
#pragma pack(pop)

static_assert(sizeof(PipelineSnapshot) == 48, "PipelineSnapshot must be 48 bytes");

// ES: DETECCIÓN DE ANOMALÍAS: tipos de anomalía y el registro de una anomalía
//     (tipo, jugador origen, cuándo se detectó, descripción y si está resuelta).
// ══════════════════════════════════════════════════════════════════════════════
// ANOMALY DETECTION
// ══════════════════════════════════════════════════════════════════════════════

enum class AnomalyType : uint8_t {
    SpawnQueueMismatch = 0x01,
    CanSpawnStuck      = 0x02,
    EntityGhostMismatch= 0x03,
    HookNotFiring      = 0x04,
    PhaseStuck         = 0x05,
    PingSpike          = 0x06,
    SnapshotStale      = 0x07,
};

struct PipelineAnomaly {
    AnomalyType   type;
    PlayerID      sourcePlayer;
    uint32_t      detectedAt;     // Timestamp ms
    std::string   description;
    bool          resolved = false;
};

// ES: Ayuda: nombre de la fase de carga para mostrarlo en el HUD/volcados.
// Helper: phase name for display
inline const char* GetPhaseName(uint8_t phase) {
    switch (phase) {
    case 0:  return "Idle";
    case 1:  return "InitLoad";
    case 2:  return "ZoneTrans";
    case 3:  return "SpawnLoad";
    default: return "???";
    }
}

} // namespace kmp
