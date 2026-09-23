// ES: Implementación de los hooks de tiempo. Hook_TimeUpdate intercepta la función
//     de avance del tiempo del juego (TimeUpdate, RVA 0x214B50, prólogo `mov rax,rsp`),
//     captura el TimeManager, escala deltaTime con la velocidad del servidor en clientes
//     no-host y dispara Core::OnGameTick. En Steam esa función nunca se ejecuta: el tick
//     real lo lleva el hook de Present (render_hooks.cpp) y el guard de 4 ms de
//     OnGameTick evitaría el doble procesado si algún día empezara a dispararse.
// EN: Implementation of the time hooks. Hook_TimeUpdate intercepts the game's time
//     advance function (TimeUpdate, RVA 0x214B50, `mov rax,rsp` prologue), captures the
//     TimeManager, scales deltaTime by the server speed on non-host clients and fires
//     Core::OnGameTick. On Steam that function never runs: the real tick is driven by the
//     Present hook (render_hooks.cpp), and OnGameTick's 4 ms guard would prevent double
//     processing should it ever start firing.
#include "time_hooks.h"
#include "../core.h"
#include "kmp/hook_manager.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <Windows.h>

namespace kmp::time_hooks {

// ES: Firma de TimeUpdate: (this = TimeManager del juego, delta de tiempo en segundos).
// EN: TimeUpdate signature: (this = game TimeManager, time delta in seconds).
using TimeUpdateFn = void(__fastcall*)(void* timeManager, float deltaTime);

// ES: Trampolín al original y últimos valores de hora/velocidad recibidos del servidor.
// EN: Trampoline to the original and last time/speed values received from the server.
static TimeUpdateFn s_origTimeUpdate = nullptr;
static float s_serverTimeOfDay = 0.5f;
static float s_serverGameSpeed = 1.0f;
static bool  s_hasServerTime = false;

// ES: Puntero al TimeManager capturado en la primera llamada del hook (para escribir directo).
// EN: Captured time manager pointer for direct writes
static void* s_timeManager = nullptr;

// ES: Ver time_hooks.h. Offsets del TimeManager: +0x08 = hora del día (float 0-1),
//     +0x10 = velocidad de juego (float). Si no hay TimeManager, solo guarda los valores.
// EN: See time_hooks.h. TimeManager offsets: +0x08 = time of day (float 0-1),
//     +0x10 = game speed (float). Without a TimeManager it only stores the values.
void SetServerTime(float timeOfDay, float gameSpeed) {
    s_serverTimeOfDay = timeOfDay;
    s_serverGameSpeed = gameSpeed;
    s_hasServerTime = true;

    // ES: Si tenemos el puntero al TimeManager, escribir la hora directamente.
    // EN: If we have the time manager pointer, write time directly
    if (s_timeManager) {
        Memory::Write(reinterpret_cast<uintptr_t>(s_timeManager) + 0x08, timeOfDay);
        Memory::Write(reinterpret_cast<uintptr_t>(s_timeManager) + 0x10, gameSpeed);
    }
}

// ES: Lee la hora del día (TimeManager+0x08); 0.5 (mediodía) si no hay TimeManager.
// EN: Reads the time of day (TimeManager+0x08); 0.5 (noon) if there is no TimeManager.
float GetTimeOfDay() {
    if (!s_timeManager) return 0.5f;
    float tod = 0.5f;
    Memory::Read(reinterpret_cast<uintptr_t>(s_timeManager) + 0x08, tod);
    return tod;
}

// ES: Lee la velocidad de juego (TimeManager+0x10); 1.0 si no hay TimeManager.
// EN: Reads the game speed (TimeManager+0x10); 1.0 if there is no TimeManager.
float GetGameSpeed() {
    if (!s_timeManager) return 1.0f;
    float speed = 1.0f;
    Memory::Read(reinterpret_cast<uintptr_t>(s_timeManager) + 0x10, speed);
    return speed;
}

// ES: Escribe la hora del día en TimeManager+0x08; false si no hay TimeManager o falla.
// EN: Writes the time of day to TimeManager+0x08; false if no TimeManager or on failure.
bool WriteTimeOfDay(float timeOfDay) {
    if (!s_timeManager) return false;
    return Memory::Write(reinterpret_cast<uintptr_t>(s_timeManager) + 0x08, timeOfDay);
}

// ES: True si el hook ya capturó el TimeManager.
// EN: True if the hook has already captured the TimeManager.
bool HasTimeManager() {
    return s_timeManager != nullptr;
}

// ES: Detour de TimeUpdate. Antes del original: captura el TimeManager y, en clientes
//     no-host con hora del servidor, multiplica deltaTime por la velocidad del servidor.
//     Llama al original con SEH. Después: si hay conexión, dispara Core::OnGameTick; si
//     no, solo deja trazas con OutputDebugStringA. Corre en el hilo de lógica del juego.
// EN: TimeUpdate detour. Before the original: captures the TimeManager and, on non-host
//     clients with server time, multiplies deltaTime by the server speed. Calls the
//     original under SEH. Afterwards: when connected, fires Core::OnGameTick; otherwise
//     it only emits OutputDebugStringA traces. Runs on the game logic thread.
static void __fastcall Hook_TimeUpdate(void* timeManager, float deltaTime) {
    // ES: Capturar el puntero al TimeManager en la primera llamada.
    // EN: Capture the time manager pointer on first call
    if (!s_timeManager) {
        s_timeManager = timeManager;
        spdlog::info("time_hooks: Captured time manager at 0x{:X}",
                     reinterpret_cast<uintptr_t>(timeManager));
    }

    auto& core = Core::Get();
    if (core.IsConnected() && !core.IsHost() && s_hasServerTime) {
        // ES: Cliente: escalar el delta con la velocidad que controla el servidor para
        //     que el paso del tiempo vaya sincronizado con él.
        // EN: Client: override delta time with server-controlled speed.
        // This keeps the client's time progression synced with the server.
        deltaTime *= s_serverGameSpeed;
    }

    __try {
        s_origTimeUpdate(timeManager, deltaTime);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("time_hooks: TimeUpdate trampoline CRASHED!");
        return;
    }

    // ES: Si hay conexión, disparar el tick del mod (OnGameTick).
    // EN: If connected, trigger the game tick
    static int s_timeHookCallCount = 0;
    s_timeHookCallCount++;
    if (core.IsConnected()) {
        if (s_timeHookCallCount <= 5) {
            char buf[128];
            sprintf_s(buf, "KMP: Hook_TimeUpdate calling OnGameTick (call #%d, dt=%.4f)\n",
                      s_timeHookCallCount, deltaTime);
            OutputDebugStringA(buf);
        }
        core.OnGameTick(deltaTime);
    } else {
        // ES: Trazar las 5 primeras llamadas sin conexión y luego 1 de cada 3000, para
        //     confirmar que el hook sigue vivo.
        // EN: Log first 5 non-connected calls then every 3000th to confirm hook is alive
        if (s_timeHookCallCount <= 5 || s_timeHookCallCount % 3000 == 0) {
            char buf[128];
            sprintf_s(buf, "KMP: Hook_TimeUpdate #%d — NOT connected, skipping OnGameTick\n",
                      s_timeHookCallCount);
            OutputDebugStringA(buf);
        }
    }
}

// ES: Instala el hook sobre la dirección de TimeUpdate resuelta por el escáner
//     (GameFunctions). Si no se resolvió, no hace nada y devuelve true igualmente.
// EN: Installs the hook on the TimeUpdate address resolved by the scanner
//     (GameFunctions). If unresolved, it does nothing and still returns true.
bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    auto& hookMgr = HookManager::Get();

    if (funcs.TimeUpdate) {
        if (hookMgr.InstallAt("TimeUpdate",
                              reinterpret_cast<uintptr_t>(funcs.TimeUpdate),
                              &Hook_TimeUpdate, &s_origTimeUpdate)) {
            // ES: NO marcar TimeHookActive: en Steam el juego nunca llama a la función de
            //     RVA 0x214B50; OnGameTick lo mueve el Present de render_hooks. Si empezara
            //     a dispararse, el guard de 4 ms de OnGameTick evita el doble procesado.
            // EN: Do NOT set TimeHookActive — the function at RVA 0x214B50 is never called
            // by the game on Steam builds. render_hooks Present drives OnGameTick instead.
            // If TimeUpdate starts firing, the 4ms dedup guard in OnGameTick prevents
            // double-processing.
            spdlog::info("time_hooks: TimeUpdate hook installed (not claiming active — Present drives OnGameTick)");
        }
    }

    spdlog::info("time_hooks: Installed (TimeUpdate={})", funcs.TimeUpdate != nullptr);
    return true;
}

// ES: Quita el hook TimeUpdate.
// EN: Removes the TimeUpdate hook.
void Uninstall() {
    HookManager::Get().Remove("TimeUpdate");
}

} // namespace kmp::time_hooks
