// ES: Implementación de los hooks de zonas. Hook_ZoneLoad y Hook_ZoneUnload llaman al
//     original con SEH (SafeCall + HookHealth) y, si hay conexión, meten un evento en un
//     buffer circular sin bloqueos. ProcessDeferredZoneEvents(), desde OnGameTick, envía
//     C2S_ZoneRequest en las cargas y, en las descargas, limpia interpolación, cupos de
//     spawn y el registro de entidades de esa zona. El antiguo hook BuildingPlace de este
//     módulo está desactivado (el activo vive en building_hooks.cpp).
// EN: Implementation of the zone hooks. Hook_ZoneLoad and Hook_ZoneUnload call the
//     original under SEH (SafeCall + HookHealth) and, when connected, push an event into a
//     lock-free ring buffer. ProcessDeferredZoneEvents(), from OnGameTick, sends
//     C2S_ZoneRequest on loads and, on unloads, cleans interpolation, spawn caps and the
//     entity registry for that zone. This module's old BuildingPlace hook is disabled (the
//     active one lives in building_hooks.cpp).
#include "world_hooks.h"
#include "entity_hooks.h"
#include "../core.h"
#include "../game/game_types.h"
#include "kmp/hook_manager.h"
#include "kmp/protocol.h"
#include "kmp/memory.h"
#include "kmp/safe_hook.h"
#include <spdlog/spdlog.h>
#include <atomic>

namespace kmp::world_hooks {

// ES: Firmas de carga/descarga de zona: (gestor de zonas, coordenada X, coordenada Y de la celda).
// EN: Zone load/unload signatures: (zone manager, cell X coordinate, cell Y coordinate).
using ZoneLoadFn = void(__fastcall*)(void* zoneMgr, int zoneX, int zoneY);
using ZoneUnloadFn = void(__fastcall*)(void* zoneMgr, int zoneX, int zoneY);

// ES: Trampolines a los originales.
// EN: Trampolines to the originals.
static ZoneLoadFn      s_origZoneLoad   = nullptr;
static ZoneUnloadFn    s_origZoneUnload = nullptr;

// ES: Salud de cada hook: cuenta crashes en la llamada al original (usado por SafeCall).
// EN: Per-hook health: counts crashes when calling the original (used by SafeCall).
// ── Hook Health ──
static HookHealth s_zoneLoadHealth{"ZoneLoad"};
static HookHealth s_zoneUnloadHealth{"ZoneUnload"};

// ES: Contadores de diagnóstico de llamadas.
// ── Diagnostic Counters ──
static std::atomic<int> s_zoneLoadCount{0};
static std::atomic<int> s_zoneUnloadCount{0};

// ═══════════════════════════════════════════════════════════════════════════
// ES: COLA DIFERIDA DE EVENTOS DE ZONA. Los hooks de zona pueden dispararse dentro de
//     detours MovRaxRsp; usar spdlog + PacketWriter + SendReliable ahí corrompe el heap o
//     bloquea. Todo el trabajo pesado se aplaza a ProcessDeferredZoneEvents() (OnGameTick).
//  DEFERRED ZONE EVENT QUEUE
//  Zone hooks may fire inside MovRaxRsp detours. spdlog + PacketWriter +
//  SendReliable inside the detour causes heap corruption / deadlock.
//  Defer all heavy work to ProcessDeferredZoneEvents() called from OnGameTick.
// ═══════════════════════════════════════════════════════════════════════════

// ES: Tipo de evento (carga/descarga) y evento con las coordenadas de la celda.
// EN: Event type (load/unload) and event with the cell coordinates.
enum class ZoneEventType : uint8_t { Load, Unload };

struct DeferredZoneEvent {
    ZoneEventType type;
    int32_t zoneX;
    int32_t zoneY;
};

// ES: Buffer circular de 32 huecos, un productor (hook) y un consumidor (OnGameTick).
// EN: 32-slot ring buffer, single producer (hook) and single consumer (OnGameTick).
static constexpr int ZONE_RING_SIZE = 32;
static DeferredZoneEvent s_zoneRing[ZONE_RING_SIZE];
static std::atomic<int> s_zoneWriteIdx{0};
static std::atomic<int> s_zoneReadIdx{0};

// ES: Añade un evento al buffer sin reservar memoria. Devuelve false (y lo descarta) si está lleno.
// EN: Pushes an event into the ring without allocating. Returns false (dropping it) when full.
static bool PushZoneEvent(const DeferredZoneEvent& evt) {
    int writeIdx = s_zoneWriteIdx.load(std::memory_order_relaxed);
    int nextIdx = (writeIdx + 1) % ZONE_RING_SIZE;
    if (nextIdx == s_zoneReadIdx.load(std::memory_order_acquire)) return false;
    s_zoneRing[writeIdx] = evt;
    s_zoneWriteIdx.store(nextIdx, std::memory_order_release);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// ES: CUERPOS DE LOS HOOKS: trabajo mínimo. Sin spdlog, sin PacketWriter, sin SendReliable.
//  HOOK BODIES — minimal work only. No spdlog, no PacketWriter, no SendReliable.
// ═══════════════════════════════════════════════════════════════════════════

// ES: Detour de ZoneLoad (hilo del juego que carga zonas). Llama primero al original
//     (la carga siempre debe ocurrir) y después, si hay conexión, encola un evento Load.
// EN: ZoneLoad detour (game thread that loads zones). Calls the original first (loading
//     must always happen) and then, when connected, queues a Load event.
static void __fastcall Hook_ZoneLoad(void* zoneMgr, int zoneX, int zoneY) {
    s_zoneLoadCount.fetch_add(1, std::memory_order_relaxed);

    // ES: Llamada al trampolín protegida con SEH: la carga de zona del juego siempre debe seguir.
    // SEH-protected trampoline call — game zone loading must always proceed
    SafeCall_Void_PtrII(reinterpret_cast<void*>(s_origZoneLoad),
                         zoneMgr, zoneX, zoneY, &s_zoneLoadHealth);

    // ES: Encolar el evento de carga para procesarlo más tarde (sin memoria dinámica).
    // Queue zone load event for deferred processing (no heap allocation)
    if (Core::Get().IsConnected()) {
        DeferredZoneEvent evt{ZoneEventType::Load, zoneX, zoneY};
        PushZoneEvent(evt);
    }
}

// ES: Detour de ZoneUnload. Llama al original ANTES de quitar entidades y encola un
//     evento Unload si hay conexión.
// EN: ZoneUnload detour. Calls the original BEFORE removing entities and queues an
//     Unload event when connected.
static void __fastcall Hook_ZoneUnload(void* zoneMgr, int zoneX, int zoneY) {
    s_zoneUnloadCount.fetch_add(1, std::memory_order_relaxed);

    // ES: Llamada protegida con SEH; tiene que ir ANTES de quitar las entidades.
    // SEH-protected trampoline call — must run BEFORE removing entities
    SafeCall_Void_PtrII(reinterpret_cast<void*>(s_origZoneUnload),
                         zoneMgr, zoneX, zoneY, &s_zoneUnloadHealth);

    // ES: Encolar el evento de descarga para procesarlo más tarde.
    // Queue zone unload event for deferred processing
    if (Core::Get().IsConnected()) {
        DeferredZoneEvent evt{ZoneEventType::Unload, zoneX, zoneY};
        PushZoneEvent(evt);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// ES: PROCESADO DIFERIDO: se llama desde Core::OnGameTick (contexto seguro).
//  DEFERRED PROCESSING — called from Core::OnGameTick (safe context)
// ═══════════════════════════════════════════════════════════════════════════

// ES: Consume hasta 8 eventos por tick. Sin conexión, vacía la cola sin procesarla.
//     Load -> envía C2S_ZoneRequest(x, y) fiable. Unload -> para cada entidad de la zona
//     descuenta el cupo de spawn de su dueño (si es remota) y borra su interpolación;
//     después elimina esas entidades del registro.
// EN: Consumes up to 8 events per tick. When disconnected, drains the queue unprocessed.
//     Load -> sends a reliable C2S_ZoneRequest(x, y). Unload -> for each entity in the zone
//     decrements its owner's spawn cap (if remote) and drops its interpolation; then removes
//     those entities from the registry.
void ProcessDeferredZoneEvents() {
    auto& core = Core::Get();
    if (!core.IsConnected()) {
        // ES: Vaciar sin procesar.
        // Drain without processing
        DeferredZoneEvent discard;
        int readIdx = s_zoneReadIdx.load(std::memory_order_relaxed);
        while (readIdx != s_zoneWriteIdx.load(std::memory_order_acquire)) {
            readIdx = (readIdx + 1) % ZONE_RING_SIZE;
        }
        s_zoneReadIdx.store(readIdx, std::memory_order_release);
        return;
    }

    int processed = 0;
    while (processed < 8) {
        int readIdx = s_zoneReadIdx.load(std::memory_order_relaxed);
        if (readIdx == s_zoneWriteIdx.load(std::memory_order_acquire)) break;
        DeferredZoneEvent evt = s_zoneRing[readIdx];
        s_zoneReadIdx.store((readIdx + 1) % ZONE_RING_SIZE, std::memory_order_release);
        processed++;

        if (evt.type == ZoneEventType::Load) {
            spdlog::info("world_hooks: [deferred] ZoneLoad ({}, {})", evt.zoneX, evt.zoneY);
            PacketWriter writer;
            writer.WriteHeader(MessageType::C2S_ZoneRequest);
            writer.WriteI32(evt.zoneX);
            writer.WriteI32(evt.zoneY);
            core.GetClient().SendReliable(writer.Data(), writer.Size());
        } else {
            spdlog::info("world_hooks: [deferred] ZoneUnload ({}, {})", evt.zoneX, evt.zoneY);
            // ES: ARREGLO BUG 2+3: limpiar estado de interpolación y cupos de spawn de las entidades
            //     de la zona antes de quitarlas del registro en bloque.
            // BUG 2+3 FIX: Clean up interpolation state and spawn caps for
            // entities in the zone before bulk-removing them from the registry.
            ZoneCoord zone(evt.zoneX, evt.zoneY);
            auto zoneEntities = core.GetEntityRegistry().GetEntitiesInZone(zone);
            for (EntityID eid : zoneEntities) {
                auto info = core.GetEntityRegistry().GetInfo(eid);
                if (info.has_value() && info->isRemote) {
                    entity_hooks::DecrementSpawnCount(info->ownerPlayerId);
                }
                core.GetInterpolation().RemoveEntity(eid);
            }
            core.GetEntityRegistry().RemoveEntitiesInZone(zone);
        }
    }
}

// ES: Instala ZoneLoad y ZoneUnload en las direcciones del escáner (si existen).
// EN: Installs ZoneLoad and ZoneUnload at the scanner addresses (if present).
bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    auto& hookMgr = HookManager::Get();

    if (funcs.ZoneLoad) {
        hookMgr.InstallAt("ZoneLoad",
                          reinterpret_cast<uintptr_t>(funcs.ZoneLoad),
                          &Hook_ZoneLoad, &s_origZoneLoad);
    }
    if (funcs.ZoneUnload) {
        hookMgr.InstallAt("ZoneUnload",
                          reinterpret_cast<uintptr_t>(funcs.ZoneUnload),
                          &Hook_ZoneUnload, &s_origZoneUnload);
    }
    // ES: Hook BuildingPlace DESACTIVADO aquí: firma sin verificar y crashes al cargar zonas.
    // BuildingPlace hook DISABLED — signature unverified, zone-load crashes
    spdlog::info("world_hooks: Installed (zone hooks only, building SKIPPED)");
    return true;
}

// ES: Quita ambos hooks de zona.
// EN: Removes both zone hooks.
void Uninstall() {
    HookManager::Get().Remove("ZoneLoad");
    HookManager::Get().Remove("ZoneUnload");
}

} // namespace kmp::world_hooks
