// ES: Estructuras de datos por frame que comparten el hilo del juego y los
//     workers de fondo (doble buffer en SyncOrchestrator): posiciones leídas de
//     las entidades locales, resultados interpolados de las remotas y el paquete
//     de posiciones ya serializado.
// EN: Per-frame data structures shared between the game thread and the
//     background workers (double-buffered in SyncOrchestrator): positions read
//     from local entities, interpolated results for remote ones and the
//     already-serialized position packet.
#pragma once
#include "kmp/types.h"
#include <cstdint>
#include <vector>

namespace kmp {

// ES: Posición cacheada leída de una entidad local por el worker de fondo
//     (dirty = se ha movido más que KMP_POS_CHANGE_THRESHOLD y hay que enviarla).
// EN:
// Cached position data read from local entities by background worker
struct CachedEntityPos {
    EntityID netId    = INVALID_ENTITY;
    Vec3     position;
    Quat     rotation;
    float    speed    = 0.f;
    uint8_t  animState = 0;
    bool     dirty    = false; // Moved beyond KMP_POS_CHANGE_THRESHOLD
};

// ES: Resultado interpolado de una entidad remota, calculado por el worker de fondo.
// EN:
// Interpolated result for a remote entity, computed by background worker
struct CachedRemoteResult {
    EntityID netId     = INVALID_ENTITY;
    Vec3     position;
    Quat     rotation;
    uint8_t  moveSpeed = 0;
    uint8_t  animState = 0;
    bool     valid     = false;
};

// ES: Contenedor de datos de un frame (uno de los dos buffers del doble buffer).
// EN:
// Per-frame double-buffered data container
struct FrameData {
    std::vector<CachedEntityPos>    localEntities;
    std::vector<CachedRemoteResult> remoteResults;
    std::vector<uint8_t>            packetBytes; // Pre-built position update packet

    bool ready = false;

    // ES: Vacía el buffer para reutilizarlo en el siguiente frame.
    // EN: Empties the buffer so it can be reused next frame.
    void Clear() {
        localEntities.clear();
        remoteResults.clear();
        packetBytes.clear();
        ready = false;
    }
};

} // namespace kmp
