#pragma once
#include "kmp/types.h"
#include "kmp/protocol.h"
#include <vector>
#include <mutex>
#include <string>

namespace kmp {

// Stores spawn packets that arrived before ClientPhase::GameReady
// These get processed when HandleAllPlayersReady() is called
struct DeferredSpawn {
    uint32_t entityId;
    uint32_t generation;
    uint8_t type;
    uint32_t ownerId;
    uint32_t templateId;
    float posX, posY, posZ;
    uint32_t compressedQuat;
    uint32_t factionId;
    std::string templateName;
    float health[7];
    bool hasExtendedHealth;
    bool isAlive;
    float timestamp;
};

class DeferredSpawnQueue {
public:
    // Queue a spawn that arrived too early
    static void Queue(const DeferredSpawn& spawn);

    // Process all queued spawns (called from HandleAllPlayersReady)
    static void ProcessAll();

    // Clear all queued spawns (called on disconnect)
    static void Clear();

    // Get queue size for debugging
    static size_t Size();

private:
    static std::vector<DeferredSpawn> s_queue;
    static std::mutex s_mutex;
};

// Process a single deferred spawn (implemented in packet_handler.cpp)
void ProcessDeferredSpawn(const DeferredSpawn& spawn);

} // namespace kmp
