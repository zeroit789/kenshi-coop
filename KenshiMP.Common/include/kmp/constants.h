// ES: Constantes globales compartidas por cliente, servidor y herramientas:
//     versión de protocolo, puertos, tick rate, interpolación, canales ENet,
//     límites de ancho de banda, timeouts, sistema de zonas y rangos de IDs.
// EN: Global constants shared by client, server and tools:
//     protocol version, ports, tick rate, interpolation, ENet channels,
//     bandwidth limits, timeouts, zone system and ID ranges.
#pragma once
#include <cstdint>

namespace kmp {

// ES: Protocolo: versión (cliente y servidor deben coincidir), puerto por defecto,
//     máximo de jugadores y longitud máxima de nombre.
// EN: Protocol
constexpr uint32_t KMP_PROTOCOL_VERSION = 1;
constexpr uint16_t KMP_DEFAULT_PORT     = 27800;
constexpr int      KMP_MAX_PLAYERS      = 16;
constexpr int      KMP_MAX_NAME_LENGTH  = 31;

// ES: Frecuencia de sincronización de estado (ticks por segundo) y su intervalo.
// EN: Tick rates
constexpr int   KMP_TICK_RATE           = 20;      // 20 Hz state sync
constexpr int   KMP_TICK_INTERVAL_MS    = 1000 / KMP_TICK_RATE; // 50ms
constexpr float KMP_TICK_INTERVAL_SEC   = 1.0f / KMP_TICK_RATE;

// ES: Interpolación: retardo del búfer (adaptativo entre MIN y MAX) y nº de snapshots por entidad.
// EN: Interpolation
constexpr float KMP_INTERP_DELAY_SEC    = 0.1f;    // 100ms default interpolation buffer
constexpr int   KMP_MAX_SNAPSHOTS       = 8;        // Ring buffer per entity (spec: 8)
constexpr float KMP_INTERP_DELAY_MIN    = 0.05f;   // 50ms minimum adaptive delay
constexpr float KMP_INTERP_DELAY_MAX    = 0.2f;    // 200ms maximum adaptive delay

// ES: Extrapolación y corrección: cuánto se extrapola sin datos nuevos y cuándo se
//     corrige suavemente o se teletransporta según la distancia de error.
// EN: Extrapolation & snap correction
constexpr float KMP_EXTRAP_MAX_SEC      = 0.25f;   // 250ms max extrapolation beyond last snapshot
constexpr float KMP_SNAP_THRESHOLD_MIN  = 5.0f;    // Below this: smooth blend correction
constexpr float KMP_SNAP_THRESHOLD_MAX  = 50.0f;   // Above this: instant teleport
constexpr float KMP_SNAP_CORRECT_SEC    = 0.15f;   // 150ms snap correction blend duration
constexpr float KMP_JITTER_EMA_ALPHA    = 0.1f;    // EMA smoothing factor for jitter estimation

// ES: Red: canales ENet (0 = fiable ordenado, 1 = fiable sin orden, 2 = no fiable secuenciado).
// EN: Networking
constexpr int   KMP_CHANNEL_COUNT       = 3;
constexpr int   KMP_CHANNEL_RELIABLE_ORDERED   = 0;
constexpr int   KMP_CHANNEL_RELIABLE_UNORDERED = 1;
constexpr int   KMP_CHANNEL_UNRELIABLE_SEQ     = 2;

// ES: Límites de ancho de banda del host ENet (dimensionados para 16 jugadores a 20 Hz).
// EN: Bandwidth limits (sized for 16 players × 20 Hz position updates)
constexpr uint32_t KMP_UPSTREAM_LIMIT   = 2 * 1024 * 1024; // 2 MB/s (server→client fan-out)
constexpr uint32_t KMP_DOWNSTREAM_LIMIT = 2 * 1024 * 1024; // 2 MB/s (client→server headroom)

// ES: Tiempos de espera: conexión, keepalive y desconexión por inactividad.
// EN: Timeouts
constexpr uint32_t KMP_CONNECT_TIMEOUT_MS  = 5000;
constexpr uint32_t KMP_KEEPALIVE_INTERVAL  = 1000;  // 1 second
constexpr uint32_t KMP_TIMEOUT_MS          = 10000;  // 10 seconds

// ES: Sistema de zonas: el mundo se divide en celdas de KMP_ZONE_SIZE; cada cliente
//     recibe las entidades de su zona y las vecinas (radio de interés 1 = rejilla 3x3).
// EN: Zone system: the world is split into KMP_ZONE_SIZE cells; each client receives
//     entities in its zone and neighbors (interest radius 1 = 3x3 grid).
constexpr float KMP_ZONE_SIZE           = 750.f;   // Meters per zone (estimated)
constexpr int   KMP_INTEREST_RADIUS     = 1;       // ±1 zone (3x3 grid)
constexpr float KMP_AUTHORITY_HYSTERESIS = 64.0f;  // Units before zone authority transfer

// ES: Umbrales mínimos de cambio para enviar una actualización de posición/rotación.
// EN: Position sync thresholds
constexpr float KMP_POS_CHANGE_THRESHOLD = 0.1f;   // Minimum movement to send update
constexpr float KMP_ROT_CHANGE_THRESHOLD = 0.01f;  // Minimum rotation change

// ES: Límites de entidades sincronizadas.
// EN: Entity limits
constexpr int KMP_MAX_ENTITIES_PER_ZONE  = 512;
constexpr int KMP_MAX_SYNC_ENTITIES      = 2048;   // Total synced entities per client

// ES: Particionado de rangos de IDs de entidad por tipo (jugadores, NPCs, edificios,
//     contenedores, escuadras) para saber el tipo solo con el ID.
// EN: ID range partitioning (spec §3.6)
constexpr uint32_t KMP_ID_PLAYER_MIN     = 1;
constexpr uint32_t KMP_ID_PLAYER_MAX     = 255;
constexpr uint32_t KMP_ID_NPC_MIN        = 256;
constexpr uint32_t KMP_ID_NPC_MAX        = 8191;
constexpr uint32_t KMP_ID_BUILDING_MIN   = 8192;
constexpr uint32_t KMP_ID_BUILDING_MAX   = 16383;
constexpr uint32_t KMP_ID_CONTAINER_MIN  = 16384;
constexpr uint32_t KMP_ID_CONTAINER_MAX  = 24575;
constexpr uint32_t KMP_ID_SQUAD_MIN      = 24576;
constexpr uint32_t KMP_ID_SQUAD_MAX      = 32767;

} // namespace kmp
