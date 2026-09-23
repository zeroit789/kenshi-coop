// ES: Implementación del bypass de spawn de escuadras. Hook_SquadSpawnCheck intercepta la
//     función del juego que decide si una escuadra ("activePlatoon") debe generar sus NPC.
//     Si hay spawns pendientes (cola del SpawnManager o cola local) y las banderas del
//     activePlatoon dicen "saltar", las pone a false, llama al original (que entonces crea
//     el NPC) y después restaura los valores. Offsets tomados de un mod de investigación
//     (sin verificar de forma independiente). Corre en el hilo de lógica del juego; sin
//     spdlog dentro del detour (solo OutputDebugStringA con buffer en pila).
// EN: Implementation of the squad spawn bypass. Hook_SquadSpawnCheck intercepts the game
//     function that decides whether a squad ("activePlatoon") should spawn its NPCs. If
//     spawns are pending (SpawnManager queue or local queue) and the activePlatoon flags say
//     "skip", it sets them to false, calls the original (which then creates the NPC) and
//     restores the values afterwards. Offsets come from a research mod (not independently
//     verified). Runs on the game logic thread; no spdlog inside the detour (only
//     OutputDebugStringA with a stack buffer).
#include "squad_spawn_hooks.h"
#include "../core.h"
#include "../game/game_types.h"
#include "../game/spawn_manager.h"
#include "../hooks/entity_hooks.h"
#include "../hooks/ai_hooks.h"
#include "kmp/hook_manager.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <queue>
#include <mutex>
#include <atomic>

namespace kmp::squad_spawn_hooks {

// ES: Petición de spawn de la cola local: GameData (plantilla) y posición.
// EN: Local queue spawn request: GameData (template) and position.
// ── Spawn queue ──
struct SquadSpawnRequest {
    void* gameData;
    Vec3 position;
};

// ES: Cola local protegida por mutex y contadores de éxitos/bypass.
// EN: Mutex-guarded local queue and success/bypass counters.
static std::mutex s_queueMutex;
static std::queue<SquadSpawnRequest> s_spawnQueue;
static std::atomic<int> s_successCount{0};
static std::atomic<int> s_bypassCount{0};

// ES: Offsets dentro de la estructura activePlatoon del juego (RE de un mod de investigación):
//     +0xF0 bool "saltar spawn", +0x58 bool segunda condición de salto, +0x250 puntero que
//     debe ser 0 para aplicar el bypass, +0x78 puntero a la platoon (escuadra), +0xA0 líder
//     (CharacterHuman*). Los dos últimos no se usan en este fichero.
// EN: Offsets inside the game's activePlatoon struct (RE from a research mod): +0xF0 bool
//     "skip spawn", +0x58 bool second skip condition, +0x250 pointer that must be 0 for the
//     bypass, +0x78 platoon (squad) pointer, +0xA0 leader (CharacterHuman*). The last two
//     are not used in this file.
// ── activePlatoon struct offsets (from research mod RE) ──
static constexpr int OFFSET_SKIP_CHECK_1 = 0xF0;   // bool: if 1, skip spawning
static constexpr int OFFSET_SKIP_CHECK_2 = 0x58;   // bool: if >0, also skip
static constexpr int OFFSET_SKIP_CHECK_3 = 0x250;  // void*: must be 0 for bypass
static constexpr int OFFSET_SQUAD_PTR    = 0x78;    // platoon*
static constexpr int OFFSET_LEADER       = 0xA0;    // CharacterHuman* leader

// ES: Firma de la comprobación de spawn: (contexto desconocido, activePlatoon) y su trampolín.
// EN: Spawn check signature: (unknown context, activePlatoon) and its trampoline.
// ── Hook function type ──
using SquadSpawnCheckFn = void(__fastcall*)(void* context, void* activePlatoon);
static SquadSpawnCheckFn s_origSquadSpawnCheck = nullptr;

// ES: Lecturas/escrituras de memoria protegidas con SEH (devuelven false si la dirección no es válida).
// EN: SEH-protected memory reads/writes (return false if the address is invalid).
// ── SEH-safe struct reads/writes ──
static bool SEH_ReadBool(uintptr_t addr, bool& out) {
    __try {
        out = *reinterpret_cast<bool*>(addr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SEH_WriteBool(uintptr_t addr, bool val) {
    __try {
        *reinterpret_cast<bool*>(addr) = val;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SEH_ReadPtr(uintptr_t addr, uintptr_t& out) {
    __try {
        out = *reinterpret_cast<uintptr_t*>(addr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
}

// ES: Valores originales guardados para restaurarlos tras el bypass. Es una sola ranura
//     global: asume que la función no es reentrante.
// EN: Original values saved to restore them after the bypass. It is a single global slot:
//     assumes the function is not re-entrant.
// Saved state for restoration after bypass
struct SavedChecks {
    bool check1;
    bool check2;
    uintptr_t platoonAddr;
    bool valid;
};
static SavedChecks s_savedChecks = {};

// ES: Detour de la comprobación de spawn. Antes: si hay pendientes y el puntero parece
//     válido, aplica el bypass (check3 == 0 y check1 == true -> check1/check2 = false).
//     Llama al original (sin SEH). Después: restaura las banderas y suma un éxito.
// EN: Spawn check detour. Before: if spawns are pending and the pointer looks valid, apply
//     the bypass (check3 == 0 and check1 == true -> check1/check2 = false). Calls the
//     original (no SEH). After: restore the flags and count a success.
static void __fastcall Hook_SquadSpawnCheck(void* context, void* activePlatoon) {
    uintptr_t apAddr = reinterpret_cast<uintptr_t>(activePlatoon);

    // ES: Mirar AMBAS colas: la de red del spawn_manager (principal) y la local (respaldo).
    //     entity_hooks secuestra NPCs de la cola del spawn_manager; aquí solo se FUERZA que el
    //     juego cree el NPC cuando lo saltaría.
    // Check BOTH queues: spawn_manager's network queue (primary) and our own (fallback).
    // Network packets queue to spawn_manager; entity_hooks hijacks NPCs from that queue.
    // Our job is to FORCE NPC creation when the game would otherwise skip spawning.
    bool hasPending = false;
    {
        auto& spawnMgr = Core::Get().GetSpawnManager();
        hasPending = spawnMgr.HasPendingSpawns();
    }
    if (!hasPending) {
        std::lock_guard lock(s_queueMutex);
        hasPending = !s_spawnQueue.empty();
    }

    if (hasPending && apAddr > 0x10000 && apAddr < 0x00007FFFFFFFFFFF) {
        bool check1 = false, check2 = false;
        uintptr_t check3 = 0;

        SEH_ReadBool(apAddr + OFFSET_SKIP_CHECK_1, check1);
        SEH_ReadBool(apAddr + OFFSET_SKIP_CHECK_2, check2);
        SEH_ReadPtr(apAddr + OFFSET_SKIP_CHECK_3, check3);

        // ES: Condición del mod de investigación: check3 == 0 Y check1 == 1 (normalmente saltaría).
        // Research mod condition: check3 == 0 AND check1 == 1 (would normally skip)
        if (check3 == 0 && check1 == true) {
            int bypassNum = s_bypassCount.fetch_add(1) + 1;
            // ES: Nada de spdlog en el detour: solo OutputDebugStringA con buffer en pila, sin heap.
            // NO spdlog inside detour — OutputDebugStringA only (stack buffer, no heap)
            {
                char dbg[128];
                sprintf_s(dbg, "KMP: squad bypass #%d platoon=0x%llX\n",
                          bypassNum, (unsigned long long)apAddr);
                OutputDebugStringA(dbg);
            }

            s_savedChecks.check1 = check1;
            s_savedChecks.check2 = check2;
            s_savedChecks.platoonAddr = apAddr;
            s_savedChecks.valid = true;

            // ES: Invertir las banderas para forzar el spawn; el NPC Hijack de entity_hooks cogerá el
            //     personaje resultante de la cola del spawn_manager.
            // Flip checks to force spawn — entity_hooks NPC Hijack will grab
            // the resulting character from spawn_manager's queue
            SEH_WriteBool(apAddr + OFFSET_SKIP_CHECK_1, false);
            SEH_WriteBool(apAddr + OFFSET_SKIP_CHECK_2, false);

            // ES: Sacar una entrada de la cola local si la hay (ruta heredada).
            // Drain our local queue if it had entries (legacy path)
            {
                std::lock_guard lock(s_queueMutex);
                if (!s_spawnQueue.empty()) {
                    s_spawnQueue.pop();
                }
            }
        }
    }

    // ES: Llamar al original: spawnea si las comprobaciones pasan.
    // Call original — spawns if checks pass
    s_origSquadSpawnCheck(context, activePlatoon);

    // ES: Restaurar los valores originales de las banderas.
    // Restore original check values
    if (s_savedChecks.valid && s_savedChecks.platoonAddr == apAddr) {
        SEH_WriteBool(apAddr + OFFSET_SKIP_CHECK_1, s_savedChecks.check1);
        SEH_WriteBool(apAddr + OFFSET_SKIP_CHECK_2, s_savedChecks.check2);
        s_savedChecks.valid = false;
        s_successCount.fetch_add(1);
        OutputDebugStringA("KMP: squad bypass complete — NPC should exist for hijack\n");
    }
}

// ES: Añade una petición a la cola local (bajo mutex) y la registra en el log.
// EN: Adds a request to the local queue (under the mutex) and logs it.
void QueueSquadSpawn(void* gameData, const Vec3& position) {
    std::lock_guard lock(s_queueMutex);
    s_spawnQueue.push({gameData, position});
    spdlog::info("squad_spawn_hooks: Queued spawn at ({:.0f},{:.0f},{:.0f}), pending={}",
                 position.x, position.y, position.z, s_spawnQueue.size());
}

// ES: Tamaño de la cola local.
// EN: Local queue size.
int GetPendingCount() {
    std::lock_guard lock(s_queueMutex);
    return static_cast<int>(s_spawnQueue.size());
}

// ES: Número de bypass completados.
// EN: Number of completed bypasses.
int GetSuccessCount() {
    return s_successCount.load();
}

// ES: Instala el hook en la dirección SquadSpawnBypass resuelta por el escáner.
// EN: Installs the hook at the scanner-resolved SquadSpawnBypass address.
bool Install() {
    auto& core = Core::Get();
    auto& hookMgr = HookManager::Get();
    auto& funcs = core.GetGameFunctions();

    if (!funcs.SquadSpawnBypass) {
        spdlog::warn("squad_spawn_hooks: SquadSpawnBypass address not resolved — hook not installed");
        return false;
    }

    uintptr_t targetAddr = reinterpret_cast<uintptr_t>(funcs.SquadSpawnBypass);
    spdlog::info("squad_spawn_hooks: Installing at 0x{:X}", targetAddr);

    if (!hookMgr.InstallAt("SquadSpawnBypass", targetAddr,
                            &Hook_SquadSpawnCheck, &s_origSquadSpawnCheck)) {
        spdlog::error("squad_spawn_hooks: Failed to install SquadSpawnBypass hook");
        return false;
    }

    spdlog::info("squad_spawn_hooks: Installed successfully");
    return true;
}

// ES: Quita el hook.
// EN: Removes the hook.
void Uninstall() {
    HookManager::Get().Remove("SquadSpawnBypass");
}

} // namespace kmp::squad_spawn_hooks
