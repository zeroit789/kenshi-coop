// ES: Implementación del pool de workers de fondo (TaskOrchestrator), con
//     protección SEH para que un fallo de acceso a memoria del juego en un
//     worker no tumbe el proceso entero.
// EN: Background worker pool (TaskOrchestrator) implementation, with SEH
//     protection so a game-memory access violation on a worker does not bring
//     down the whole process.
#include "task_orchestrator.h"
#include <spdlog/spdlog.h>
#include <windows.h>

namespace kmp {

// ES: Envoltorio SEH para ejecutar tareas en los workers. try/catch de C++ NO
//     captura violaciones de acceso (excepciones estructuradas); sin esto, un AV en
//     BackgroundReadEntities o BackgroundInterpolate mata el proceso en silencio.
//     Con SEH se captura, se registra y el worker sigue con las siguientes tareas.
//     No puede contener objetos C++ con destructor (regla de MSVC para __try), por
//     eso el std::function se pasa como puntero.
// EN:
// SEH wrapper for task execution on worker threads.
// C++ try/catch does NOT catch access violations (structured exceptions).
// Without this, an AV in BackgroundReadEntities or BackgroundInterpolate
// terminates the entire process silently. With SEH, the AV is caught,
// logged, and the worker thread continues processing future tasks.
//
// This function MUST NOT contain C++ objects with destructors (MSVC rule).
// The std::function is passed by pointer (no construction inside __try).
static bool SEH_ExecuteWorkerTask(void (*executor)(void*), void* context) {
    __try {
        executor(context);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        char buf[256];
        sprintf_s(buf, "KMP: TaskOrchestrator worker SEH caught exception 0x%08lX "
                       "(likely AV in game memory read on background thread)\n", code);
        OutputDebugStringA(buf);
        return false;
    }
}

// ES: Trampolín: llama al std::function<void()> recibido como void*.
// EN:
// Trampoline: calls std::function<void()> via void* cast
static void ExecuteFnPtr(void* ctx) {
    auto* fn = static_cast<std::function<void()>*>(ctx);
    (*fn)();
}

// ES: Arranca numWorkers hilos; no hace nada si ya estaba en marcha.
// EN: Starts numWorkers threads; no-op if already running.
void TaskOrchestrator::Start(int numWorkers) {
    if (m_running.exchange(true)) return; // Already running

    spdlog::info("TaskOrchestrator: Starting {} worker threads", numWorkers);
    m_workers.reserve(numWorkers);
    for (int i = 0; i < numWorkers; ++i) {
        m_workers.emplace_back(&TaskOrchestrator::WorkerLoop, this);
    }
}

// ES: Para los workers, los espera (join), descarta las tareas pendientes y
//     despierta a cualquier hilo bloqueado en WaitForFrameWork().
// EN: Stops the workers, joins them, discards pending tasks and wakes any
//     thread blocked in WaitForFrameWork().
void TaskOrchestrator::Stop() {
    if (!m_running.exchange(false)) return; // Already stopped

    // ES: Despertar a todos los workers para que salgan.
    // EN:
    // Wake all workers so they exit
    m_taskCV.notify_all();

    for (auto& w : m_workers) {
        if (w.joinable()) w.join();
    }
    m_workers.clear();

    // ES: Vaciar las tareas que queden en la cola.
    // EN:
    // Drain any remaining tasks
    {
        std::lock_guard lock(m_taskMutex);
        while (!m_tasks.empty()) m_tasks.pop();
    }
    m_pendingFrameWork.store(0);
    // Despertar a cualquier hilo bloqueado en WaitForFrameWork() durante el teardown — antes solo
    // se notificaba m_taskCV, nunca m_frameCV, así que un waiter podía quedarse colgado para
    // siempre si Stop() corría mientras esperaba frame work.
    // EN: Wake any thread blocked in WaitForFrameWork() during teardown. Previously only
    //     m_taskCV was notified, never m_frameCV, so a waiter could hang forever if
    //     Stop() ran while it was waiting for frame work.
    { std::lock_guard<std::mutex> lk(m_frameMutex); }
    m_frameCV.notify_all();

    spdlog::info("TaskOrchestrator: Stopped");
}

// ES: Encola una tarea normal y despierta a un worker.
// EN: Enqueues a normal task and wakes one worker.
void TaskOrchestrator::Post(std::function<void()> task) {
    {
        std::lock_guard lock(m_taskMutex);
        m_tasks.push({std::move(task), false});
    }
    m_taskCV.notify_one();
}

// ES: Encola una tarea de frame: primero incrementa el contador de pendientes para
//     que WaitForFrameWork() no pueda ver 0 antes de que la tarea exista.
// EN: Enqueues a frame task: bumps the pending counter first so WaitForFrameWork()
//     cannot observe 0 before the task exists.
void TaskOrchestrator::PostFrameWork(std::function<void()> task) {
    m_pendingFrameWork.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(m_taskMutex);
        m_tasks.push({std::move(task), true});
    }
    m_taskCV.notify_one();
}

// ES: Espera (hilo del juego) a que el contador de tareas de frame llegue a 0.
// EN: Waits (game thread) for the frame-task counter to reach 0.
void TaskOrchestrator::WaitForFrameWork() {
    std::unique_lock lock(m_frameMutex);
    m_frameCV.wait(lock, [this] {
        return m_pendingFrameWork.load(std::memory_order_acquire) <= 0;
    });
}

// ES: Bucle del worker: espera tareas, las ejecuta bajo SEH y, si eran de frame,
//     decrementa el contador y avisa al hilo del juego cuando llega a 0.
// EN: Worker loop: waits for tasks, runs them under SEH and, for frame tasks,
//     decrements the counter and notifies the game thread when it reaches 0.
void TaskOrchestrator::WorkerLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        Task task;
        {
            std::unique_lock lock(m_taskMutex);
            m_taskCV.wait(lock, [this] {
                return !m_tasks.empty() || !m_running.load(std::memory_order_acquire);
            });

            // ES: Salir solo cuando se ha pedido parar y la cola está vacía.
            // EN: Exit only once a stop was requested and the queue is empty.
            if (!m_running.load(std::memory_order_acquire) && m_tasks.empty()) {
                return;
            }

            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        // ES: Ejecutar la tarea con protección SEH (AV al leer memoria del juego).
        //     Sin esto, un AV en un worker terminaría todo el proceso.
        // EN:
        // Execute the task with SEH + C++ exception protection.
        // SEH catches structured exceptions (access violations from game memory reads).
        // Without this, an AV on a worker thread terminates the entire process.
        if (!SEH_ExecuteWorkerTask(ExecuteFnPtr, &task.work)) {
            spdlog::error("TaskOrchestrator: Worker caught structured exception "
                          "(AV in game memory) — task skipped, worker continues");
        }

        // ES: Si era trabajo de frame, decrementar el contador y avisar al hilo del juego.
        // EN:
        // If this was frame work, decrement counter and notify game thread
        if (task.isFrameWork) {
            if (m_pendingFrameWork.fetch_sub(1, std::memory_order_acq_rel) <= 1) {
                // Lost-wakeup fix: sin tomar m_frameMutex aquí, hay una ventana de nanosegundos
                // donde el waiter (WaitForFrameWork) evalúa el predicado justo antes de este
                // fetch_sub/notify y se bloquea DESPUÉS del notify -> notificación perdida ->
                // cuelgue permanente (nadie vuelve a postear frame work tras un Reset()). El
                // lock-vacío basta: fuerza a que el notify espere a que el waiter esté o bien ya
                // dentro de wait() (lo despierta) o bien aún no haya entrado (verá el predicado
                // ya actualizado al re-evaluar). No protege datos, solo serializa con el waiter.
                // EN: Lost-wakeup fix: without taking m_frameMutex here there is a tiny window
                //     where the waiter evaluates the predicate right before this fetch_sub/notify
                //     and blocks AFTER the notify -> lost notification -> permanent hang (no one
                //     posts frame work again after a Reset()). The empty lock is enough: it forces
                //     the notify to wait until the waiter is either already inside wait() (woken)
                //     or has not entered yet (it will see the updated predicate). It protects no
                //     data, it only serializes with the waiter.
                { std::lock_guard<std::mutex> lk(m_frameMutex); }
                m_frameCV.notify_all();
            }
        }
    }
}

} // namespace kmp
