// ES: Implementación de los hooks de IA. Ambos detours SIEMPRE llaman al original (con
//     SEH): el juego desreferencia el controlador de IA y el árbol de comportamiento sin
//     comprobar nulos, así que no se pueden suprimir. Si el personaje es de un jugador
//     remoto se marca en s_remoteControlled; otros módulos consultan ese conjunto para no
//     dejar que la IA mueva a esos personajes. Corren en el hilo del juego que crea el
//     personaje; el conjunto se protege con un mutex.
// EN: Implementation of the AI hooks. Both detours ALWAYS call the original (under SEH):
//     the game dereferences the AI controller and behavior tree without null checks, so
//     they cannot be suppressed. If the character belongs to a remote player it is marked
//     in s_remoteControlled; other modules query that set so the AI does not move those
//     characters. They run on the game thread that creates the character; the set is
//     guarded by a mutex.
#include "ai_hooks.h"
#include "kmp/hook_manager.h"
#include "kmp/patterns.h"
#include "../core.h"
#include "../game/game_types.h"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <mutex>

namespace kmp::ai_hooks {

// ES: Firmas: AICreate(personaje, facción) -> controlador de IA; AIPackages(personaje, paquete).
// EN: Signatures: AICreate(character, faction) -> AI controller; AIPackages(character, package).
// ── Function typedefs ──
using AICreateFn   = void*(__fastcall*)(void* character, void* faction);
using AIPackagesFn = void(__fastcall*)(void* character, void* aiPackage);

// ES: Trampolines a los originales y contadores de llamadas (para log muestreado).
// EN: Trampolines to the originals and call counters (for sampled logging).
// ── State ──
static AICreateFn   s_origAICreate   = nullptr;
static AIPackagesFn s_origAIPackages = nullptr;
static int s_createCount = 0;
static int s_packageCount = 0;

// ES: Conjunto de personajes controlados en remoto: sus decisiones de IA las sustituye la
//     red. El CONTROLADOR de IA se mantiene válido (¡nunca nullptr!) para que el motor no
//     crashee al usarlo; solo se suprimen las DECISIONES.
// ── Remote-controlled character tracking ──
// Characters in this set have their AI decisions overridden by network input.
// The AI CONTROLLER is kept valid (no nullptr!) so the engine doesn't crash
// when downstream code dereferences it. Only the AI DECISIONS are suppressed.
static std::unordered_set<void*> s_remoteControlled;
static std::mutex s_remoteMutex;

// ES: Añade el personaje al conjunto (bajo mutex) y lo registra en el log.
// EN: Adds the character to the set (under the mutex) and logs it.
void MarkRemoteControlled(void* character) {
    std::lock_guard lock(s_remoteMutex);
    s_remoteControlled.insert(character);
    spdlog::info("ai_hooks: Marked character 0x{:X} as remote-controlled ({} total)",
                 (uintptr_t)character, s_remoteControlled.size());
}

// ES: Quita el personaje del conjunto.
// EN: Removes the character from the set.
void UnmarkRemoteControlled(void* character) {
    std::lock_guard lock(s_remoteMutex);
    s_remoteControlled.erase(character);
}

// ES: True si el personaje está marcado como remoto.
// EN: True if the character is marked as remote.
bool IsRemoteControlled(void* character) {
    std::lock_guard lock(s_remoteMutex);
    return s_remoteControlled.count(character) > 0;
}

// EN: Change 3.2: clears the remote-controlled set under the same lock. Called on
//     disconnect / hot save reload: the marked pointers belong to Characters from the
//     previous session that the engine already freed; leaving them marked would suppress
//     the AI of new local NPCs if the engine reuses those addresses.
// Cambio 3.2: vacía el set de remote-controlled bajo el mismo lock.
// Llamado en desconexión / recarga de save en caliente: los punteros marcados pertenecen
// a Characters de la sesión anterior que el motor ya liberó; dejarlos marcados suprimiría
// la IA de NPCs locales nuevos si el motor recicla esas direcciones.
void ClearRemoteControlled() {
    std::lock_guard lock(s_remoteMutex);
    s_remoteControlled.clear();
}

// EN: Diagnostics (read-only): current size of the remote-controlled set. Meant for
//     [DIAG-REMOTE]: with the host alive and alone (no peers) it should be 0; >0 with the
//     host marked is an anomaly that would block its combat/AI.
// Diagnóstico (solo lectura): tamaño actual del set de remote-controlled.
// Pensado para [DIAG-REMOTE]: si el host está vivo y solo (sin peers) este valor
// debería ser 0; >0 con el host marcado = anomalía que bloquearía su combate/IA.
size_t RemoteControlledCount() {
    std::lock_guard lock(s_remoteMutex);
    return s_remoteControlled.size();
}

// ── Hooks ──

// ES: Detour de AICreate. Llama SIEMPRE al original (SEH) y, si hay conexión y el
//     personaje está registrado como remoto, lo marca como controlado en remoto. Devuelve
//     el controlador creado por el juego (nullptr solo si el original crasheó).
// EN: AICreate detour. ALWAYS calls the original (SEH) and, when connected and the
//     character is registered as remote, marks it as remote-controlled. Returns the
//     controller created by the game (nullptr only if the original crashed).
static void* __fastcall Hook_AICreate(void* character, void* faction) {
    s_createCount++;

    // ES: Llamar SIEMPRE al AICreate original: todo personaje necesita un controlador de IA
    //     válido. Devolver nullptr aquí era la causa raíz de crashes al interactuar con
    //     remotos (combate, pathfinding, animación y selección en la UI lo usan sin comprobar).
    // ALWAYS call the original AICreate — every character needs a valid AI controller.
    // Returning nullptr here was the root cause of crashes when interacting with
    // remote characters: downstream code dereferences the AI controller without
    // null checks (combat, pathfinding, animation state transitions, UI selection).
    void* result = nullptr;
    __try {
        result = s_origAICreate(character, faction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("ai_hooks: AICreate crashed");
        return nullptr;
    }

    auto& core = Core::Get();
    if (!core.IsConnected()) return result;

    // ES: Si es un personaje de un jugador remoto, marcarlo para sustituir sus decisiones de
    //     IA (movimiento/tareas los dirige la red). El controlador sigue siendo VÁLIDO.
    // Check if this is a remote player's character — if so, mark it for
    // AI decision override (movement/tasks blocked, driven by network instead).
    // The AI controller itself stays VALID — only decisions are suppressed.
    auto& registry = core.GetEntityRegistry();
    EntityID netId = registry.GetNetId(character);
    if (netId != INVALID_ENTITY) {
        auto info = registry.GetInfo(netId);
        if (info.has_value() && info->isRemote) {
            MarkRemoteControlled(character);
            spdlog::info("ai_hooks: AICreate for remote entity {} — AI controller CREATED "
                         "(decisions will be overridden by network), char=0x{:X}",
                         netId, (uintptr_t)character);
        }
    }

    if (s_createCount % 100 == 1) {
        spdlog::debug("ai_hooks: AICreate #{} (char=0x{:X}, faction=0x{:X})",
                       s_createCount, (uintptr_t)character, (uintptr_t)faction);
    }

    return result;
}

// ES: Detour de AIPackages (diagnóstico). Siempre deja cargar el paquete de IA (SEH) y
//     solo loguea si el personaje es remoto; no suprime nada.
// EN: AIPackages detour (diagnostic). Always lets the AI package load (SEH) and only logs
//     when the character is remote; it suppresses nothing.
static void __fastcall Hook_AIPackages(void* character, void* aiPackage) {
    s_packageCount++;

    // ES: Dejar SIEMPRE que carguen los paquetes: el árbol de comportamiento debe existir o el
    //     motor crashea al consultarlo, también en remotos (solo se sustituyen sus decisiones).
    // ALWAYS let AI packages load — the character needs valid behavior trees
    // to prevent crashes when the engine queries them. Even for remote characters,
    // the behavior tree structure must exist; we just override the actual decisions.
    __try {
        s_origAIPackages(character, aiPackage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("ai_hooks: AIPackages crashed");
        return;
    }

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    // ES: Log para personajes remotos (solo diagnóstico, sin supresión).
    // Log for remote characters (diagnostic only — no suppression)
    auto& registry = core.GetEntityRegistry();
    EntityID netId = registry.GetNetId(character);
    if (netId != INVALID_ENTITY) {
        auto info = registry.GetInfo(netId);
        if (info.has_value() && info->isRemote) {
            spdlog::debug("ai_hooks: AI packages LOADED for remote entity {} "
                           "(behavior tree valid, decisions overridden)",
                           netId);
        }
    }

    if (s_packageCount % 200 == 1) {
        spdlog::debug("ai_hooks: AIPackages #{} (char=0x{:X}, pkg=0x{:X})",
                       s_packageCount, (uintptr_t)character, (uintptr_t)aiPackage);
    }
}

// ── Install / Uninstall ──

// ES: Instala los dos hooks en las direcciones del escáner (GameFunctions).
// EN: Installs both hooks at the scanner addresses (GameFunctions).
bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    auto& hooks = HookManager::Get();
    int installed = 0;

    if (funcs.AICreate) {
        if (hooks.InstallAt("AICreate", reinterpret_cast<uintptr_t>(funcs.AICreate),
                            &Hook_AICreate, &s_origAICreate)) {
            installed++;
            spdlog::info("ai_hooks: AICreate hook installed");
        }
    }

    if (funcs.AIPackages) {
        if (hooks.InstallAt("AIPackages", reinterpret_cast<uintptr_t>(funcs.AIPackages),
                            &Hook_AIPackages, &s_origAIPackages)) {
            installed++;
            spdlog::info("ai_hooks: AIPackages hook installed");
        }
    }

    spdlog::info("ai_hooks: {}/2 hooks installed", installed);
    return installed > 0;
}

// ES: Quita los hooks instalados, olvida los trampolines y vacía el conjunto de remotos.
// EN: Removes installed hooks, forgets the trampolines and clears the remote set.
void Uninstall() {
    auto& hooks = HookManager::Get();
    if (s_origAICreate)   hooks.Remove("AICreate");
    if (s_origAIPackages) hooks.Remove("AIPackages");
    s_origAICreate = nullptr;
    s_origAIPackages = nullptr;

    // ES: Vaciar el seguimiento de remotos.
    // Clear remote tracking
    std::lock_guard lock(s_remoteMutex);
    s_remoteControlled.clear();
}

} // namespace kmp::ai_hooks
