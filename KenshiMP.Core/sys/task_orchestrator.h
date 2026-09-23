// ES: Pool de hilos de trabajo de fondo del cliente. Ejecuta tareas sueltas
//     ("fire-and-forget") y tareas "de frame" que el hilo del juego espera antes
//     de intercambiar los buffers de FrameData.
// EN: Client background worker thread pool. Runs loose fire-and-forget tasks
//     and "frame" tasks that the game thread waits for before swapping the
//     FrameData buffers.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace kmp {

// ES: Orquestador de tareas: cola única protegida por mutex + N hilos worker.
// EN: Task orchestrator: a single mutex-guarded queue plus N worker threads.
class TaskOrchestrator {
public:
    // ES: Arranca N workers (idempotente) / los para, vacía la cola y despierta a quien espere.
    // EN: Starts N workers (idempotent) / stops them, drains the queue and wakes any waiter.
    void Start(int numWorkers = 2);
    void Stop();

    // ES: Encola una tarea general de fondo (sin esperar su resultado).
    // EN:
    // Enqueue a general background task (fire-and-forget)
    void Post(std::function<void()> task);

    // ES: Encola una tarea que DEBE terminar antes del siguiente intercambio de frame.
    //     El hilo del juego llama a WaitForFrameWork() para bloquearse hasta que acaben todas.
    // EN:
    // Enqueue a task that MUST complete before the next frame swap.
    // Game thread calls WaitForFrameWork() to block until all frame tasks are done.
    void PostFrameWork(std::function<void()> task);

    // ES: Bloquea hasta que terminen todas las tareas PostFrameWork (desde el hilo del juego).
    // EN:
    // Block until all PostFrameWork tasks have completed (called from game thread)
    void WaitForFrameWork();

    // ES: Indica si los workers están en marcha.
    // EN: Whether the workers are running.
    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    // ES: Bucle de cada worker: saca tareas de la cola y las ejecuta protegidas por SEH.
    // EN: Each worker's loop: pops tasks from the queue and runs them under SEH protection.
    void WorkerLoop();

    // ES: Tarea encolada; isFrameWork indica si cuenta para WaitForFrameWork().
    // EN: Queued task; isFrameWork tells whether it counts for WaitForFrameWork().
    struct Task {
        std::function<void()> work;
        bool isFrameWork = false;
    };

    // ES: Hilos, cola de tareas y su sincronización.
    // EN: Threads, task queue and its synchronization.
    std::vector<std::thread>  m_workers;
    std::queue<Task>          m_tasks;
    std::mutex                m_taskMutex;
    std::condition_variable   m_taskCV;

    // ES: Contador de tareas de frame pendientes y la variable de condición del waiter.
    // EN: Pending frame-task counter and the waiter's condition variable.
    std::atomic<int>          m_pendingFrameWork{0};
    std::mutex                m_frameMutex;
    std::condition_variable   m_frameCV;

    std::atomic<bool>         m_running{false};
};

} // namespace kmp
