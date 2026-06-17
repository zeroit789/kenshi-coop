#pragma once
#include "kmp/types.h"
#include "kmp/messages.h"
#include <unordered_map>
#include <vector>
#include <mutex>

namespace kmp {

struct PendingSnapshot {
    CharacterPosition position;
    uint32_t sourcePlayerId;
    float timestamp;
};

class PendingSnapshotQueue {
public:
    // Queue a snapshot for an entity that hasn't spawned yet
    static void Queue(const CharacterPosition& pos, uint32_t sourcePlayer);

    // Flush all pending snapshots for an entity (called when spawn completes)
    static void FlushForEntity(uint32_t entityId);

    // Clean up old pending snapshots (older than maxAge seconds)
    static void CleanupOld(float currentTime, float maxAge = 10.0f);

private:
    static std::unordered_map<uint32_t, std::vector<PendingSnapshot>> s_pending;
    static std::mutex s_mutex;
};

} // namespace kmp
