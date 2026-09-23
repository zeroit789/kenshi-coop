// ES: Implementación de los hooks de edificios. Patrón común de los cinco detours: intentar
//     "recuperar" el contador de crashes, llamar al original con SEH (si crashea, contar y al
//     llegar a MAX_CRASHES quitar el hook), y después, si hay conexión y no se está cargando,
//     enviar el paquete de red: C2S_BuildRequest (colocación), C2S_EntityDespawnReq
//     (destrucción), C2S_BuildDismantle (desmontaje); construcción y reparación solo loguean.
//     Este BuildingPlace es distinto del desactivado en world_hooks.cpp. Corren en el hilo de
//     lógica del juego.
// EN: Implementation of the building hooks. Common pattern for the five detours: try to
//     "recover" the crash counter, call the original under SEH (on a crash, count it and at
//     MAX_CRASHES remove the hook), then, when connected and not loading, send the network
//     packet: C2S_BuildRequest (placement), C2S_EntityDespawnReq (destruction),
//     C2S_BuildDismantle (dismantle); construction and repair only log. This BuildingPlace is
//     different from the disabled one in world_hooks.cpp. They run on the game logic thread.
#include "building_hooks.h"
#include "kmp/hook_manager.h"
#include "kmp/patterns.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/memory.h"
#include "../core.h"
#include "../game/game_types.h"
#include <spdlog/spdlog.h>
#include <Windows.h>

namespace kmp::building_hooks {

// ES: Firmas supuestas (no todas verificadas): colocar(mundo, edificio, x, y, z),
//     destruido(edificio), desmontar(edificio), construir(edificio, progreso), reparar(edificio, cantidad).
// EN: Assumed signatures (not all verified): place(world, building, x, y, z),
//     destroyed(building), dismantle(building), construct(building, progress), repair(building, amount).
// ── Function typedefs ──
using BuildingPlaceFn     = void(__fastcall*)(void* world, void* building, float x, float y, float z);
using BuildingDestroyedFn = void(__fastcall*)(void* building);
using BuildingDismantleFn = void(__fastcall*)(void* building);
using BuildingConstructFn = void(__fastcall*)(void* building, float progress);
using BuildingRepairFn    = void(__fastcall*)(void* building, float amount);

// ES: Estado: trampolines, contadores de llamadas y bandera de carga (no atómica).
// EN: State: trampolines, call counters and loading flag (not atomic).
// ── State ──
static BuildingPlaceFn     s_origBuildingPlace     = nullptr;
static BuildingDestroyedFn s_origBuildingDestroyed = nullptr;
static BuildingDismantleFn s_origBuildingDismantle = nullptr;
static BuildingConstructFn s_origBuildingConstruct = nullptr;
static BuildingRepairFn    s_origBuildingRepair    = nullptr;
static int s_placeCount = 0;
static int s_destroyCount = 0;
static int s_dismantleCount = 0;
static int s_constructCount = 0;
static int s_repairCount = 0;
static bool s_loading = false;

// ES: Contadores de crashes: el hook se quita tras MAX_CRASHES (función equivocada del
//     escáner). Recuperación: si pasan RECOVERY_SECONDS sin crashes, el contador vuelve a 0
//     para que fallos puntuales no desactiven la sincronización toda la sesión. Una vez
//     quitado el hook, la recuperación ya no lo reinstala.
// Crash counters — auto-disable hooks after MAX_CRASHES crashes (wrong function matched by scanner)
// Recovery: after RECOVERY_SECONDS with no crashes, reset counter to 0 so temporary glitches
// don't permanently disable building sync for the rest of the session.
static int s_placeCrashCount = 0;
static int s_destroyCrashCount = 0;
static int s_dismantleCrashCount = 0;
static int s_constructCrashCount = 0;
static int s_repairCrashCount = 0;
static constexpr int MAX_CRASHES = 10;
static constexpr double RECOVERY_SECONDS = 60.0;

// ES: Instante del último crash de cada hook (GetTickCount64, ms) para la recuperación.
// Last crash timestamps (GetTickCount64 ms) for recovery logic
static ULONGLONG s_placeCrashTime = 0;
static ULONGLONG s_destroyCrashTime = 0;
static ULONGLONG s_dismantleCrashTime = 0;
static ULONGLONG s_constructCrashTime = 0;
static ULONGLONG s_repairCrashTime = 0;

// ES: Ayudante: si el contador está entre 1 y MAX_CRASHES-1 y han pasado RECOVERY_SECONDS
//     desde el último crash, lo pone a 0. Devuelve true si lo reinició.
// Helper: check if enough time has passed since last crash to reset the counter.
// Returns true if the counter was reset (caller should proceed normally).
static bool TryRecover(int& crashCount, ULONGLONG& lastCrashTime, const char* hookName) {
    if (crashCount > 0 && crashCount < MAX_CRASHES) {
        ULONGLONG now = GetTickCount64();
        if (now - lastCrashTime >= static_cast<ULONGLONG>(RECOVERY_SECONDS * 1000)) {
            spdlog::info("building_hooks: {} — no crashes for {:.0f}s, resetting crash counter ({} -> 0)",
                         hookName, RECOVERY_SECONDS, crashCount);
            crashCount = 0;
            return true;
        }
    }
    return false;
}

// ES: Envoltorios SEH de cada original (sin objetos C++ con destructor, requisito de __try).
//     Devuelven false si la llamada crasheó.
// ── SEH wrappers (no C++ objects with destructors) ──

static bool SEH_BuildingPlace(void* world, void* building, float x, float y, float z) {
    __try {
        s_origBuildingPlace(world, building, x, y, z);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SEH_BuildingDestroyed(void* building) {
    __try {
        s_origBuildingDestroyed(building);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SEH_BuildingDismantle(void* building) {
    __try {
        s_origBuildingDismantle(building);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SEH_BuildingConstruct(void* building, float progress) {
    __try {
        s_origBuildingConstruct(building, progress);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SEH_BuildingRepair(void* building, float amount) {
    __try {
        s_origBuildingRepair(building, amount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── Hooks ──

// ES: Detour de BuildingPlace (createBuilding). Tras el original, envía C2S_BuildRequest
//     con un id de plantilla local, la posición y la rotación comprimida (si el offset está
//     verificado).
// EN: BuildingPlace detour (createBuilding). After the original it sends C2S_BuildRequest
//     with a local template id, the position and the compressed rotation (if the offset is
//     verified).
static void __fastcall Hook_BuildingPlace(void* world, void* building, float x, float y, float z) {
    s_placeCount++;
    TryRecover(s_placeCrashCount, s_placeCrashTime, "BuildingPlace");

    if (!SEH_BuildingPlace(world, building, x, y, z)) {
        s_placeCrashTime = GetTickCount64();
        if (++s_placeCrashCount <= MAX_CRASHES) {
            spdlog::error("building_hooks: BuildingPlace crashed ({}/{})", s_placeCrashCount, MAX_CRASHES);
        }
        if (s_placeCrashCount == MAX_CRASHES) {
            spdlog::warn("building_hooks: BuildingPlace crashed {} times — disabling hook (wrong function matched)", MAX_CRASHES);
            HookManager::Get().Remove("BuildingPlace");
            s_origBuildingPlace = nullptr;
        }
        return;
    }

    if (s_loading) return;

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    spdlog::info("building_hooks: BuildingPlace #{} (bld=0x{:X}, pos=[{:.1f},{:.1f},{:.1f}])",
                  s_placeCount, (uintptr_t)building, x, y, z);

    // ES: Sacar el templateId del puntero a GameData del edificio. (El bloque en español que
    //     sigue explica la corrección audit-14.)
    // Extract templateId from building's GameData backpointer.
    // ⚠ CORRECCIÓN audit-14 (2026-06-19):
    //   • El GameData* del building está en +0x40 (RootObjectBase `GameData* data`), NO en +0x28.
    //     +0x28 caía dentro de displayName/sections → puntero basura (Building hereda RootObjectBase
    //     igual que Character: owner@0x10, displayName@0x18, data@0x40, pos@0x48).
    //   • gameData+0x08 NO es un "templateId" entero: es `int validity` (KenshiLib GameData).
    //     NO existe un id numérico plano en GameData+0x8. El identificador estable es el string-id
    //     FCS (offset por confirmar con CE) o el propio puntero GameData. Como el protocolo aún
    //     espera un uint32, enviamos los 4 bytes bajos del PUNTERO GameData (estable dentro de la
    //     sesión: todos los edificios de la misma plantilla comparten el mismo GameData*) en vez
    //     de `validity`. Es un id local consistente, NO portable entre máquinas (igual limitación
    //     que ItemOffsets.templateId@0x40). Ver audit-14 §2.7/§2.11 y la cola de facciones (#4).
    // EN: audit-14 FIX (2026-06-19): the building's GameData* is at +0x40 (RootObjectBase
    //     `GameData* data`), NOT +0x28 (+0x28 fell inside displayName/sections -> garbage; Building
    //     inherits RootObjectBase like Character: owner@0x10, displayName@0x18, data@0x40,
    //     pos@0x48). gameData+0x08 is NOT an integer templateId but `int validity` (KenshiLib
    //     GameData); there is no flat numeric id there. The stable identifier would be the FCS
    //     string id (offset to confirm with CE) or the GameData pointer itself. Since the protocol
    //     still expects a uint32, the low 4 bytes of the GameData POINTER are sent (stable within
    //     the session; all buildings of a template share the same GameData*). It is a consistent
    //     local id, NOT portable across machines.
    uint32_t templateId = 0;
    uintptr_t bldPtr = reinterpret_cast<uintptr_t>(building);
    auto& bldOffsets = game::GetOffsets().building;

    // GameData* del building en +0x40 (data, RootObjectBase) — antes +0x28 (⛔ basura).
    // EN: Building's GameData* at +0x40 (data, RootObjectBase) — previously +0x28 (garbage).
    uintptr_t gameData = 0;
    if (Memory::Read(bldPtr + 0x40, gameData) && gameData != 0 && gameData > 0x10000) {
        // Los 4 bytes bajos del puntero GameData como id local estable (gameData+0x08 era
        // `validity`, no un id → no usar). Identificador consistente por-sesión, no portable.
        // EN: Low 4 bytes of the GameData pointer as a stable local id (gameData+0x08 was
        //     `validity`, not an id). Per-session consistent, not portable.
        templateId = static_cast<uint32_t>(gameData & 0xFFFFFFFF);
    }

    // ES: Leer la rotación del edificio (se omite si el offset no está verificado, -1).
    // Extract rotation from building struct (skip if offset unverified)
    uint32_t compQuat = 0;
    if (bldOffsets.rotation >= 0) {
        Quat rot;
        if (Memory::Read(bldPtr + bldOffsets.rotation, rot)) {
            compQuat = rot.Compress();
        }
    } else {
        static bool s_loggedRotationSkip = false;
        if (!s_loggedRotationSkip) {
            spdlog::warn("building_hooks: BuildingPlace skipping rotation read — offset unverified (-1)");
            s_loggedRotationSkip = true;
        }
    }

    // ES: Construir y enviar C2S_BuildRequest (canal fiable).
    // EN: Build and send C2S_BuildRequest (reliable channel).
    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_BuildRequest);
    MsgBuildRequest msg{};
    msg.templateId = templateId;
    msg.posX = x;
    msg.posY = y;
    msg.posZ = z;
    msg.compressedQuat = compQuat;
    writer.WriteRaw(&msg, sizeof(msg));
    core.GetClient().SendReliable(writer.Data(), writer.Size());
}

// ES: Detour de BuildingDestroyed. Tras el original, si el edificio tiene netId, envía
//     C2S_EntityDespawnReq con motivo 1 (destruido).
// EN: BuildingDestroyed detour. After the original, if the building has a netId, sends
//     C2S_EntityDespawnReq with reason 1 (destroyed).
static void __fastcall Hook_BuildingDestroyed(void* building) {
    s_destroyCount++;
    TryRecover(s_destroyCrashCount, s_destroyCrashTime, "BuildingDestroyed");

    if (!SEH_BuildingDestroyed(building)) {
        s_destroyCrashTime = GetTickCount64();
        if (++s_destroyCrashCount <= MAX_CRASHES) {
            spdlog::error("building_hooks: BuildingDestroyed crashed ({}/{})", s_destroyCrashCount, MAX_CRASHES);
        }
        if (s_destroyCrashCount == MAX_CRASHES) {
            spdlog::warn("building_hooks: BuildingDestroyed disabled (wrong function)");
            HookManager::Get().Remove("BuildingDestroyed");
            s_origBuildingDestroyed = nullptr;
        }
        return;
    }

    if (s_loading) return;

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    spdlog::info("building_hooks: BuildingDestroyed #{} (bld=0x{:X})",
                  s_destroyCount, (uintptr_t)building);

    auto& registry = core.GetEntityRegistry();
    EntityID netId = registry.GetNetId(building);
    if (netId != INVALID_ENTITY) {
        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_EntityDespawnReq);
        writer.WriteU32(netId);
        writer.WriteU8(1); // reason: destroyed
        core.GetClient().SendReliable(writer.Data(), writer.Size());
    }
}

// ES: Detour de BuildingDismantle. Tras el original, si el edificio tiene netId, envía
//     C2S_BuildDismantle con el jugador local como autor.
// EN: BuildingDismantle detour. After the original, if the building has a netId, sends
//     C2S_BuildDismantle with the local player as the dismantler.
static void __fastcall Hook_BuildingDismantle(void* building) {
    s_dismantleCount++;
    TryRecover(s_dismantleCrashCount, s_dismantleCrashTime, "BuildingDismantle");

    if (!SEH_BuildingDismantle(building)) {
        s_dismantleCrashTime = GetTickCount64();
        if (++s_dismantleCrashCount <= MAX_CRASHES) {
            spdlog::error("building_hooks: BuildingDismantle crashed ({}/{})", s_dismantleCrashCount, MAX_CRASHES);
        }
        if (s_dismantleCrashCount == MAX_CRASHES) {
            spdlog::warn("building_hooks: BuildingDismantle disabled (wrong function)");
            HookManager::Get().Remove("BuildingDismantle");
            s_origBuildingDismantle = nullptr;
        }
        return;
    }

    if (s_loading) return;

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    spdlog::info("building_hooks: BuildingDismantle #{} (bld=0x{:X})",
                  s_dismantleCount, (uintptr_t)building);

    auto& registry = core.GetEntityRegistry();
    EntityID netId = registry.GetNetId(building);
    if (netId != INVALID_ENTITY) {
        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_BuildDismantle);
        MsgBuildDismantle msg{};
        msg.buildingId = netId;
        msg.dismantlerId = core.GetLocalPlayerId();
        writer.WriteRaw(&msg, sizeof(msg));
        core.GetClient().SendReliable(writer.Data(), writer.Size());
    }
}

// ES: Detour de BuildingConstruct (progreso de obra). Solo diagnóstico: loguea 1 de cada 50.
// EN: BuildingConstruct detour (build progress). Diagnostic only: logs 1 in 50 calls.
static void __fastcall Hook_BuildingConstruct(void* building, float progress) {
    s_constructCount++;
    TryRecover(s_constructCrashCount, s_constructCrashTime, "BuildingConstruct");

    if (!SEH_BuildingConstruct(building, progress)) {
        s_constructCrashTime = GetTickCount64();
        if (++s_constructCrashCount <= MAX_CRASHES) {
            spdlog::error("building_hooks: BuildingConstruct crashed ({}/{})", s_constructCrashCount, MAX_CRASHES);
        }
        if (s_constructCrashCount == MAX_CRASHES) {
            spdlog::warn("building_hooks: BuildingConstruct disabled (wrong function)");
            HookManager::Get().Remove("BuildingConstruct");
            s_origBuildingConstruct = nullptr;
        }
        return;
    }

    if (s_loading) return;

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    if (s_constructCount % 50 == 0) {
        spdlog::debug("building_hooks: BuildingConstruct #{} (bld=0x{:X}, progress={:.2f})",
                       s_constructCount, (uintptr_t)building, progress);
    }
}

// ES: Detour de BuildingRepair. Solo diagnóstico: loguea 1 de cada 50.
// EN: BuildingRepair detour. Diagnostic only: logs 1 in 50 calls.
static void __fastcall Hook_BuildingRepair(void* building, float amount) {
    s_repairCount++;
    TryRecover(s_repairCrashCount, s_repairCrashTime, "BuildingRepair");

    if (!SEH_BuildingRepair(building, amount)) {
        s_repairCrashTime = GetTickCount64();
        if (++s_repairCrashCount <= MAX_CRASHES) {
            spdlog::error("building_hooks: BuildingRepair crashed ({}/{})", s_repairCrashCount, MAX_CRASHES);
        }
        if (s_repairCrashCount == MAX_CRASHES) {
            spdlog::warn("building_hooks: BuildingRepair disabled (wrong function)");
            HookManager::Get().Remove("BuildingRepair");
            s_origBuildingRepair = nullptr;
        }
        return;
    }

    if (s_loading) return;

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    if (s_repairCount % 50 == 0) {
        spdlog::debug("building_hooks: BuildingRepair #{} (bld=0x{:X}, amount={:.2f})",
                       s_repairCount, (uintptr_t)building, amount);
    }
}

// ── Install / Uninstall ──

// ES: Instala cada hook cuya dirección haya resuelto el escáner (GameFunctions).
// EN: Installs every hook whose address the scanner resolved (GameFunctions).
bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    auto& hooks = HookManager::Get();
    int installed = 0;

    if (funcs.BuildingPlace) {
        if (hooks.InstallAt("BuildingPlace", reinterpret_cast<uintptr_t>(funcs.BuildingPlace),
                            &Hook_BuildingPlace, &s_origBuildingPlace)) {
            installed++;
        }
    }

    if (funcs.BuildingDestroyed) {
        if (hooks.InstallAt("BuildingDestroyed", reinterpret_cast<uintptr_t>(funcs.BuildingDestroyed),
                            &Hook_BuildingDestroyed, &s_origBuildingDestroyed)) {
            installed++;
        }
    }

    if (funcs.BuildingDismantle) {
        if (hooks.InstallAt("BuildingDismantle", reinterpret_cast<uintptr_t>(funcs.BuildingDismantle),
                            &Hook_BuildingDismantle, &s_origBuildingDismantle)) {
            installed++;
        }
    }

    if (funcs.BuildingConstruct) {
        if (hooks.InstallAt("BuildingConstruct", reinterpret_cast<uintptr_t>(funcs.BuildingConstruct),
                            &Hook_BuildingConstruct, &s_origBuildingConstruct)) {
            installed++;
        }
    }

    if (funcs.BuildingRepair) {
        if (hooks.InstallAt("BuildingRepair", reinterpret_cast<uintptr_t>(funcs.BuildingRepair),
                            &Hook_BuildingRepair, &s_origBuildingRepair)) {
            installed++;
        }
    }

    spdlog::info("building_hooks: {}/5 hooks installed", installed);
    return installed > 0;
}

// ES: Quita los hooks instalados y olvida los trampolines.
// EN: Removes installed hooks and forgets the trampolines.
void Uninstall() {
    auto& hooks = HookManager::Get();
    if (s_origBuildingPlace)     hooks.Remove("BuildingPlace");
    if (s_origBuildingDestroyed) hooks.Remove("BuildingDestroyed");
    if (s_origBuildingDismantle) hooks.Remove("BuildingDismantle");
    if (s_origBuildingConstruct) hooks.Remove("BuildingConstruct");
    if (s_origBuildingRepair)    hooks.Remove("BuildingRepair");
    s_origBuildingPlace = nullptr;
    s_origBuildingDestroyed = nullptr;
    s_origBuildingDismantle = nullptr;
    s_origBuildingConstruct = nullptr;
    s_origBuildingRepair = nullptr;
}

// ES: Activa/desactiva la supresión de envíos durante la carga.
// EN: Enables/disables send suppression during loading.
void SetLoading(bool loading) {
    s_loading = loading;
}

} // namespace kmp::building_hooks
