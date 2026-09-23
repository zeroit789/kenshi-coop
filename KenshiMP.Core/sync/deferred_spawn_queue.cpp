// ES: Implementación de DeferredSpawnQueue (almacenamiento estático + mutex).
// EN: DeferredSpawnQueue implementation (static storage + mutex).
#include "deferred_spawn_queue.h"
#include "../core.h"
#include <spdlog/spdlog.h>

namespace kmp {

// ES: Almacenamiento estático de la cola y su mutex.
// EN: Static storage for the queue and its mutex.
std::vector<DeferredSpawn> DeferredSpawnQueue::s_queue;
std::mutex DeferredSpawnQueue::s_mutex;

// ES: Añade un spawn al final de la cola.
// EN: Appends a spawn to the end of the queue.
void DeferredSpawnQueue::Queue(const DeferredSpawn& spawn) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_queue.push_back(spawn);
    spdlog::info("DeferredSpawnQueue: Queued spawn for entity {} (queue size={})",
                 spawn.entityId, s_queue.size());
}

// ES: Procesa y vacía la cola llamando a ProcessDeferredSpawn por cada entrada.
//     Ojo: mantiene el mutex bloqueado durante todo el bucle; si ProcessDeferredSpawn
//     llamara a Queue() se produciría un interbloqueo (std::mutex no es recursivo).
// EN: Processes and empties the queue by calling ProcessDeferredSpawn for each entry.
//     Note: it keeps the mutex locked for the whole loop; if ProcessDeferredSpawn
//     called Queue() it would deadlock (std::mutex is not recursive).
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

// ES: Descarta todos los spawns encolados.
// EN: Discards all queued spawns.
void DeferredSpawnQueue::Clear() {
    std::lock_guard<std::mutex> lock(s_mutex);
    size_t count = s_queue.size();
    s_queue.clear();
    if (count > 0) {
        spdlog::info("DeferredSpawnQueue: Cleared {} queued spawns", count);
    }
}

// ES: Número de spawns en cola.
// EN: Number of queued spawns.
size_t DeferredSpawnQueue::Size() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_queue.size();
}

} // namespace kmp
