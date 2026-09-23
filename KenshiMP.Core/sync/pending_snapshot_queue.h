// ES: Cola de snapshots pendientes. Guarda las actualizaciones de posición que
//     llegan para entidades que el cliente aún no ha creado (su spawn no ha llegado
//     o no se ha completado). Cuando la entidad aparece se vuelcan al interpolador.
// EN: Pending snapshot queue. Stores position updates that arrive for entities the
//     client has not created yet (their spawn has not arrived or not completed).
//     When the entity appears they are flushed into the interpolator.
#pragma once
#include "kmp/types.h"
#include "kmp/messages.h"
#include <unordered_map>
#include <vector>
#include <mutex>

namespace kmp {

// ES: Un snapshot encolado: posición recibida, jugador origen y hora de encolado
//     (SessionTime, en segundos).
// EN: One queued snapshot: received position, source player and queue time
//     (SessionTime, in seconds).
struct PendingSnapshot {
    CharacterPosition position;
    uint32_t sourcePlayerId;
    float timestamp;
};

// ES: Cola global (estática) por id de entidad, protegida por mutex.
// EN: Global (static) per-entity-id queue, protected by a mutex.
class PendingSnapshotQueue {
public:
    // ES: Encola un snapshot para una entidad que todavía no existe.
    // Queue a snapshot for an entity that hasn't spawned yet
    static void Queue(const CharacterPosition& pos, uint32_t sourcePlayer);

    // ES: Vuelca todos los snapshots pendientes de una entidad (al completarse su spawn).
    // Flush all pending snapshots for an entity (called when spawn completes)
    static void FlushForEntity(uint32_t entityId);

    // ES: Borra los snapshots pendientes más viejos que maxAge segundos.
    // Clean up old pending snapshots (older than maxAge seconds)
    static void CleanupOld(float currentTime, float maxAge = 10.0f);

private:
    static std::unordered_map<uint32_t, std::vector<PendingSnapshot>> s_pending;
    static std::mutex s_mutex;
};

} // namespace kmp
