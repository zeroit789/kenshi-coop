// ES: Implementación del hook de GameFrameUpdate (función de actualización de frame de
//     Kenshi, RVA 0x123A10, prólogo `mov rax,rsp`). El detour corre en el hilo principal
//     del juego y solo hace diagnóstico: cuenta ticks, cada 3000 ticks escribe por
//     OutputDebugStringA cuántos spawns hay pendientes y llama al original con SEH. No
//     spawnea nada ni usa spdlog (dentro de un detour MovRaxRsp no es seguro).
// EN: Implementation of the GameFrameUpdate hook (Kenshi's frame update function,
//     RVA 0x123A10, `mov rax,rsp` prologue). The detour runs on the game's main thread and
//     is diagnostic only: it counts ticks, every 3000 ticks writes pending spawn counts via
//     OutputDebugStringA, and calls the original under SEH. It spawns nothing and does not
//     use spdlog (not safe inside a MovRaxRsp detour).
#include "game_tick_hooks.h"
#include "entity_hooks.h"
#include "../core.h"
#include "../game/spawn_manager.h"
#include "../game/game_types.h"
#include "../game/player_controller.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <Windows.h>

namespace kmp::game_tick_hooks {

// ES: GameFrameUpdate empieza por `mov rax, rsp` (bytes 48 8B C4). HookManager aplica solo
//     el arreglo MovRaxRsp: un detour "naked" captura RSP al entrar y el envoltorio del
//     trampolín restaura RAX antes de entrar en el cuerpo original, para que el RBP que el
//     juego deriva de RAX sea correcto. Firma: dos punteros opacos (rcx, rdx) sin identificar.
// GameFrameUpdate starts with `mov rax, rsp` (48 8B C4).
// HookManager automatically applies the MovRaxRsp fix: a naked detour captures
// RSP at hook entry, and the trampoline wrapper restores RAX before entering
// the original function body. This ensures correct RBP derivation.
using GameFrameUpdateFn = void(__fastcall*)(void* rcx, void* rdx);

// ES: Trampolín (se usa para llamar al original) y dirección real de la función (solo diagnóstico).
// EN: Trampoline (used to call the original) and real function address (diagnostics only).
static GameFrameUpdateFn s_originalFn = nullptr; // trampoline — USED for calling
static uintptr_t s_targetAddr = 0;               // real function address (for diagnostics)

// ES: Envoltorio SEH para llamar al GameFrameUpdate original por el trampolín; si crashea,
//     traza como mucho los 5 primeros fallos y sigue.
// EN: SEH wrapper that calls the original GameFrameUpdate via the trampoline; on a crash it
//     traces at most the first 5 failures and carries on.
// ── SEH wrapper for calling original GameFrameUpdate via TRAMPOLINE ──
static void SEH_CallOriginal(GameFrameUpdateFn trampoline, void* rcx, void* rdx) {
    __try {
        trampoline(rcx, rdx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_crashCount = 0;
        if (++s_crashCount <= 5) {
            char buf[128];
            sprintf_s(buf, "KMP: GameFrameUpdate TRAMPOLINE CRASHED #%d\n", s_crashCount);
            OutputDebugStringA(buf);
        }
    }
}

// ES: Número de frames vistos por el hook.
// EN: Number of frames seen by the hook.
static std::atomic<int> s_tickCount{0};

// ES: Detour de GameFrameUpdate. Antes del original: trazas de los 5 primeros ticks y
//     diagnóstico de spawns cada 3000 ticks. Luego llama al original. Después: nada más
//     (las sondas diferidas están desactivadas).
// EN: GameFrameUpdate detour. Before the original: traces for the first 5 ticks and spawn
//     diagnostics every 3000 ticks. Then calls the original. Afterwards: nothing else
//     (deferred probes are disabled).
static void __fastcall Hook_GameFrameUpdate(void* rcx, void* rdx) {
    int tick = s_tickCount.fetch_add(1) + 1;

    // ES: Depuración: trazar cada paso en los 5 primeros ticks.
    // ── DEBUG: Log every step for first 5 ticks ──
    if (tick <= 5) {
        char buf[256];
        sprintf_s(buf, "KMP: GameFrameUpdate ENTER tick #%d rcx=0x%p rdx=0x%p\n", tick, rcx, rdx);
        OutputDebugStringA(buf);
    }

    // ES: DIAGNÓSTICO DE SPAWN (aquí no se spawnea nada). El respaldo de spawn va SOLO en
    //     Core::HandleSpawnQueue (timeout de 10 s). Antes este hook tenía un spawn directo a
    //     los 3 s que competía con el "in-place replay" de entity_hooks (necesita ~5 s tras la
    //     ráfaga de carga); se quitó para que el replay tenga prioridad.
    // ═══ SPAWN DIAGNOSTICS (no spawning here) ═══
    // Spawn fallback is handled ONLY in Core::HandleSpawnQueue (10s timeout).
    // Previously this hook also had a 3s direct spawn fallback, but it raced with
    // the safer in-place replay method in entity_hooks (which needs ~5s to settle
    // after loading burst). By removing the competing 3s spawner, in-place replay
    // gets first crack at the queue before the Core fallback kicks in at 10s.
    {
        auto& core = Core::Get();
        bool connected = core.IsConnected();

        // ES: Trazar el estado de spawn cada 3000 ticks (~20 s a 150 fps). Nada de spdlog dentro
        //     del detour MovRaxRsp: solo OutputDebugStringA.
        // Log spawn conditions every 3000 ticks (~20 seconds at 150 fps)
        // NO spdlog inside MovRaxRsp detour — use OutputDebugStringA only
        if (tick % 3000 == 0 && connected) {
            auto& spawnMgr = core.GetSpawnManager();
            size_t pendingCount = spawnMgr.GetPendingSpawnCount();
            int inPlaceCount = entity_hooks::GetInPlaceSpawnCount();
            char diagBuf[128];
            sprintf_s(diagBuf, "KMP: tick=%d pending=%zu inPlace=%d\n",
                      tick, pendingCount, inPlaceCount);
            OutputDebugStringA(diagBuf);
        }
    }

    if (tick <= 5) {
        char buf[128];
        sprintf_s(buf, "KMP: GameFrameUpdate tick #%d — about to call original (TRAMPOLINE)\n", tick);
        OutputDebugStringA(buf);
    }

    // ES: Llamar al original por el envoltorio MovRaxRsp, que cambia a la pila del llamador
    //     del juego y restaura RAX antes de entrar al cuerpo original (RBP y pila correctos).
    // Call original via MovRaxRsp trampoline wrapper.
    // The wrapper swaps to the game caller's stack and restores RAX before
    // entering the original function body — correct RBP and stack layout.
    SEH_CallOriginal(s_originalFn, rcx, rdx);

    if (tick <= 5) {
        OutputDebugStringA("KMP: GameFrameUpdate — trampoline returned OK\n");
    }

    // ES: SONDAS DIFERIDAS DESACTIVADAS: la sonda de AnimClass llenaba el log de fallos cada
    //     frame sin acertar nunca y la de PlayerControlled dependía de CharacterIterator, que
    //     también falla. Eran optimizaciones no esenciales; se quitaron como posible origen de crash.
    // ═══ DEFERRED PROBES — DISABLED ═══
    // AnimClass probing was flooding the log with failures every frame and never
    // succeeding. PlayerControlled probing relies on CharacterIterator which also
    // fails. Both are non-essential optimizations. Disabled to eliminate as crash source.

    if (tick <= 5) {
        char buf[128];
        sprintf_s(buf, "KMP: GameFrameUpdate tick #%d DONE\n", tick);
        OutputDebugStringA(buf);
    }
}

// ES: Engancha GameFrameUpdate en la dirección resuelta por el escáner (modo trampolín,
//     sin suspender hilos). Devuelve false si no hay dirección o falla la instalación.
// EN: Hooks GameFrameUpdate at the scanner-resolved address (trampoline mode, no thread
//     suspension). Returns false if there is no address or installation fails.
bool Install() {
    auto& core = Core::Get();
    auto& hookMgr = HookManager::Get();
    auto& funcs = core.GetGameFunctions();

    if (!funcs.GameFrameUpdate) {
        spdlog::warn("game_tick_hooks: GameFrameUpdate not found, skipping");
        return false;
    }

    s_targetAddr = reinterpret_cast<uintptr_t>(funcs.GameFrameUpdate);

    OutputDebugStringA("KMP: game_tick_hooks — calling InstallAt...\n");

    if (!hookMgr.InstallAt("GameFrameUpdate",
                            s_targetAddr,
                            &Hook_GameFrameUpdate, &s_originalFn)) {
        spdlog::error("game_tick_hooks: Failed to hook GameFrameUpdate");
        OutputDebugStringA("KMP: game_tick_hooks — InstallAt FAILED\n");
        return false;
    }

    char buf[128];
    sprintf_s(buf, "KMP: game_tick_hooks INSTALLED at 0x%llX\n", (unsigned long long)s_targetAddr);
    OutputDebugStringA(buf);
    spdlog::info("game_tick_hooks: Installed at 0x{:X} (trampoline mode — no thread suspension)", s_targetAddr);
    return true;
}

// ES: Quita el hook GameFrameUpdate.
// EN: Removes the GameFrameUpdate hook.
void Uninstall() {
    HookManager::Get().Remove("GameFrameUpdate");
}

} // namespace kmp::game_tick_hooks
