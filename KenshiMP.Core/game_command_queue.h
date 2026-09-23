// ES: game_command_queue.h — Cola de comandos para pasar trabajo del hilo de red al hilo de juego.
//     El hilo de red (ENet) no puede tocar memoria del juego ni objetos de Ogre directamente:
//     encola lambdas aquí y Core::OnGameTick (Step 1.5) las drena en el hilo de juego/OGRE.
// EN: game_command_queue.h — Command queue that hands work from the network thread to the game thread.
//     The network (ENet) thread must not touch game memory or Ogre objects directly:
//     it enqueues lambdas here and Core::OnGameTick (Step 1.5) drains them on the game/OGRE thread.
#pragma once
#include <functional>
#include <vector>
#include <mutex>

namespace kmp {

// ES: Comando que se ejecuta en el hilo de juego/OGRE. El hilo de red los encola y OnGameTick los drena.
// Command executed on the game/OGRE thread.
// Network thread enqueues these; OnGameTick drains them.
struct GameCommand {
    std::function<void()> execute;

    GameCommand() = default;
    GameCommand(std::function<void()> fn) : execute(std::move(fn)) {}
};

// ES: Cola thread-safe (protegida con mutex) para llevar trabajo del hilo de red al hilo de juego.
//     Crítico: TODA escritura en memoria del juego y todo acceso a objetos de escena OGRE debe pasar por aquí.
// Thread-safe command queue for marshalling network thread work to game thread.
// Critical: ALL game memory writes and OGRE scene object access must go through this.
class GameCommandQueue {
public:
    GameCommandQueue() = default;
    ~GameCommandQueue() = default;

    // ES: Encola un comando desde el hilo de red (o cualquier hilo).
    // Enqueue command from network thread (or any thread)
    void Push(GameCommand cmd) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_commands.push_back(std::move(cmd));
    }

    // ES: Drena todos los comandos en el hilo de juego. Intercambia el vector bajo el lock y ejecuta fuera
    //     de él (así los productores no se bloquean); cada comando se ejecuta a través de fn (el caller
    //     lo envuelve en SEH).
    // EN: Drains all commands on the game thread. Swaps the vector under the lock and runs outside it
    //     (so producers never block); each command runs through fn (the caller wraps it in SEH).
    // Drain all commands on game thread
    void DrainAll(const std::function<void(GameCommand&)>& fn) {
        std::vector<GameCommand> local;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_commands.empty()) return;
            local.swap(m_commands);
        }

        for (auto& cmd : local) {
            // Usar el ejecutor pasado por el caller (con SEH), no llamar directo: antes el
            // parámetro fn se IGNORABA y una excepción en una lambda abortaba el resto del tick.
            // EN: Use the executor passed by the caller (with SEH) instead of calling directly: previously the
            //     fn parameter was IGNORED and an exception inside one lambda aborted the rest of the tick.
            if (cmd.execute) fn(cmd);
        }
    }

    // ES: Número de comandos pendientes (para diagnóstico).
    // Get pending command count (for diagnostics)
    size_t Size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_commands.size();
    }

    // ES: Vacía todos los comandos pendientes (se usa al desconectar: capturan punteros de la sesión que muere).
    // EN: Clears all pending commands (used on disconnect: they capture pointers from the dying session).
    // Clear all pending commands (use on disconnect)
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_commands.clear();
    }

private:
    // ES: Mutex que protege m_commands (mutable para poder usarlo desde Size() const) y el vector de pendientes.
    // EN: Mutex guarding m_commands (mutable so Size() const can lock it) and the pending vector.
    mutable std::mutex m_mutex;
    std::vector<GameCommand> m_commands;
};

} // namespace kmp
