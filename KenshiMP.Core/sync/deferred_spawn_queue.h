// ES: Cola de spawns diferidos. Guarda los paquetes de spawn de entidades que
//     llegan antes de que el cliente esté en ClientPhase::GameReady (el juego aún
//     no puede crear personajes) y los procesa todos juntos al recibir
//     "todos los jugadores listos".
// EN: Deferred spawn queue. Stores entity spawn packets that arrive before the
//     client reaches ClientPhase::GameReady (the game cannot create characters yet)
//     and processes them all at once when "all players ready" is received.
#pragma once
#include "kmp/types.h"
#include "kmp/protocol.h"
#include <vector>
#include <mutex>
#include <string>

namespace kmp {

// ES: Guarda los paquetes de spawn que llegaron antes de ClientPhase::GameReady.
//     Se procesan cuando se llama a HandleAllPlayersReady(). Es una copia plana de
//     los campos del mensaje de spawn (id, generación, tipo, dueño, plantilla,
//     posición, rotación comprimida, facción, salud por miembro, etc.).
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
    // ES: Salud de los 7 miembros/partes del cuerpo (solo válida si hasExtendedHealth).
    // EN: Health of the 7 limbs/body parts (only valid if hasExtendedHealth).
    float health[7];
    bool hasExtendedHealth;
    bool isAlive;
    float timestamp;
};

// ES: Cola global (estática) protegida por mutex; se puede usar desde varios hilos.
// EN: Global (static) queue protected by a mutex; safe to use from several threads.
class DeferredSpawnQueue {
public:
    // ES: Encola un spawn que llegó demasiado pronto.
    // Queue a spawn that arrived too early
    static void Queue(const DeferredSpawn& spawn);

    // ES: Procesa todos los spawns encolados (se llama desde HandleAllPlayersReady).
    // Process all queued spawns (called from HandleAllPlayersReady)
    static void ProcessAll();

    // ES: Vacía la cola (se llama al desconectar).
    // Clear all queued spawns (called on disconnect)
    static void Clear();

    // ES: Devuelve el tamaño de la cola (para depuración).
    // Get queue size for debugging
    static size_t Size();

private:
    static std::vector<DeferredSpawn> s_queue;
    static std::mutex s_mutex;
};

// ES: Procesa un único spawn diferido (implementado en packet_handler.cpp).
// Process a single deferred spawn (implemented in packet_handler.cpp)
void ProcessDeferredSpawn(const DeferredSpawn& spawn);

} // namespace kmp
