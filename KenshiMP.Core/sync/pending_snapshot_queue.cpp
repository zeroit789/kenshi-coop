// ES: Implementación de PendingSnapshotQueue.
// EN: PendingSnapshotQueue implementation.
#include "pending_snapshot_queue.h"
#include "../core.h"
#include <spdlog/spdlog.h>

namespace kmp {

// ES: Almacenamiento estático: mapa id de entidad -> lista de snapshots, y su mutex.
// EN: Static storage: entity id -> snapshot list map, and its mutex.
std::unordered_map<uint32_t, std::vector<PendingSnapshot>> PendingSnapshotQueue::s_pending;
std::mutex PendingSnapshotQueue::s_mutex;

// ES: Guarda el snapshot con la hora actual de sesión como marca de tiempo.
// EN: Stores the snapshot stamped with the current session time.
void PendingSnapshotQueue::Queue(const CharacterPosition& pos, uint32_t sourcePlayer) {
    std::lock_guard<std::mutex> lock(s_mutex);

    PendingSnapshot snapshot;
    snapshot.position = pos;
    snapshot.sourcePlayerId = sourcePlayer;
    snapshot.timestamp = SessionTime();

    s_pending[pos.entityId].push_back(snapshot);

    size_t total = s_pending[pos.entityId].size();
    spdlog::debug("Queued snapshot for entity {} (total={})", pos.entityId, total);
}

// ES: Pasa al interpolador todos los snapshots guardados de la entidad y borra su
//     entrada. Se usa la hora de encolado como tiempo del snapshot.
// EN: Feeds every stored snapshot of the entity into the interpolator and removes
//     its entry. The queue time is used as the snapshot time.
void PendingSnapshotQueue::FlushForEntity(uint32_t entityId) {
    std::lock_guard<std::mutex> lock(s_mutex);

    auto it = s_pending.find(entityId);
    if (it == s_pending.end()) {
        return;
    }

    const auto& snapshots = it->second;
    size_t count = snapshots.size();

    spdlog::info("Flushing {} snapshots for entity {}", count, entityId);

    auto& interpolation = Core::Get().GetInterpolation();
    for (const auto& snapshot : snapshots) {
        const auto& pos = snapshot.position;
        Vec3 position(pos.posX, pos.posY, pos.posZ);
        Quat rotation = Quat::Decompress(pos.compressedQuat);
        interpolation.AddSnapshot(pos.entityId, snapshot.timestamp, position, rotation,
                                  pos.moveSpeed, pos.animStateId);
    }

    s_pending.erase(it);
}

// ES: Elimina snapshots anteriores a (currentTime - maxAge) y las entradas vacías.
//     Se llama periódicamente desde core.cpp (maxAge = 10 s).
// EN: Removes snapshots older than (currentTime - maxAge) and empty entries.
//     Called periodically from core.cpp (maxAge = 10 s).
void PendingSnapshotQueue::CleanupOld(float currentTime, float maxAge) {
    std::lock_guard<std::mutex> lock(s_mutex);

    float cutoffTime = currentTime - maxAge;
    size_t cleanedCount = 0;

    for (auto it = s_pending.begin(); it != s_pending.end();) {
        auto& snapshots = it->second;

        // ES: Quitar los snapshots viejos.
        // Remove old snapshots
        auto removeIt = std::remove_if(snapshots.begin(), snapshots.end(),
            [cutoffTime, &cleanedCount](const PendingSnapshot& s) {
                if (s.timestamp < cutoffTime) {
                    ++cleanedCount;
                    return true;
                }
                return false;
            });

        snapshots.erase(removeIt, snapshots.end());

        // ES: Si no quedan snapshots para esta entidad, borrar la entrada.
        // If no snapshots left for this entity, remove the entry
        if (snapshots.empty()) {
            it = s_pending.erase(it);
        } else {
            ++it;
        }
    }

    if (cleanedCount > 0) {
        spdlog::debug("Cleaned up {} old pending snapshots", cleanedCount);
    }
}

} // namespace kmp
