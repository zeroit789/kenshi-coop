#include "deferred_spawn_queue.h"
#include "../core.h"
#include <spdlog/spdlog.h>

namespace kmp {

std::vector<DeferredSpawn> DeferredSpawnQueue::s_queue;
std::mutex DeferredSpawnQueue::s_mutex;

void DeferredSpawnQueue::Queue(const DeferredSpawn& spawn) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_queue.push_back(spawn);
    spdlog::info("DeferredSpawnQueue: Queued spawn for entity {} (queue size={})",
                 spawn.entityId, s_queue.size());
}

void DeferredSpawnQueue::ProcessAll() {
    std::lock_guard<std::mutex> lock(s_mutex);

    if (s_queue.empty()) {
        spdlog::debug("DeferredSpawnQueue: No queued spawns to process");
        return;
    }

    spdlog::info("DeferredSpawnQueue: Processing {} queued spawns", s_queue.size());

    for (const auto& spawn : s_queue) {
        spdlog::info("DeferredSpawnQueue: Processing entity {} owner={} template='{}'",
                     spawn.entityId, spawn.ownerId, spawn.templateName);
        ProcessDeferredSpawn(spawn);
    }

    s_queue.clear();
    spdlog::info("DeferredSpawnQueue: All queued spawns processed");
}

void DeferredSpawnQueue::Clear() {
    std::lock_guard<std::mutex> lock(s_mutex);
    size_t count = s_queue.size();
    s_queue.clear();
    if (count > 0) {
        spdlog::info("DeferredSpawnQueue: Cleared {} queued spawns", count);
    }
}

size_t DeferredSpawnQueue::Size() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_queue.size();
}

} // namespace kmp
