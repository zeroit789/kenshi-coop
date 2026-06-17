#include "pending_snapshot_queue.h"
#include "../core.h"
#include <spdlog/spdlog.h>

namespace kmp {

std::unordered_map<uint32_t, std::vector<PendingSnapshot>> PendingSnapshotQueue::s_pending;
std::mutex PendingSnapshotQueue::s_mutex;

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

void PendingSnapshotQueue::CleanupOld(float currentTime, float maxAge) {
    std::lock_guard<std::mutex> lock(s_mutex);

    float cutoffTime = currentTime - maxAge;
    size_t cleanedCount = 0;

    for (auto it = s_pending.begin(); it != s_pending.end();) {
        auto& snapshots = it->second;

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
