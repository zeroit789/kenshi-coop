#include "faction_hooks.h"
#include "kmp/hook_manager.h"
#include "kmp/patterns.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "../core.h"
#include "../game/game_types.h"
#include "kmp/memory.h"
#include <atomic>
#include <spdlog/spdlog.h>

namespace kmp::faction_hooks {

// ── Function typedefs ──
using FactionRelationFn = void(__fastcall*)(void* factionA, void* factionB, float relation);

// ── State ──
static FactionRelationFn s_origFactionRelation = nullptr;
static int s_relationChangeCount = 0;
static std::atomic<bool> s_loading{false};
static std::atomic<bool> s_serverSourced{false};

// ── SEH wrapper ──

static bool SEH_FactionRelation(void* factionA, void* factionB, float relation) {
    __try {
        s_origFactionRelation(factionA, factionB, relation);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── Hooks ──

static void __fastcall Hook_FactionRelation(void* factionA, void* factionB, float relation) {
    s_relationChangeCount++;

    if (!SEH_FactionRelation(factionA, factionB, relation)) {
        spdlog::error("faction_hooks: FactionRelation crashed");
        return;
    }

    if (s_loading.load(std::memory_order_acquire) || s_serverSourced.load(std::memory_order_acquire)) return;

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    spdlog::info("faction_hooks: FactionRelation #{} (A=0x{:X}, B=0x{:X}, rel={:.2f})",
                  s_relationChangeCount, (uintptr_t)factionA, (uintptr_t)factionB, relation);

    uint32_t factionIdA = 0, factionIdB = 0;
    const int factionIdOffset = game::GetOffsets().faction.id;
    if (factionIdOffset < 0) {
        spdlog::warn("faction_hooks: faction.id offset not resolved (-1), skipping relation packet");
        return;
    }
    if (factionA) Memory::Read(reinterpret_cast<uintptr_t>(factionA) + factionIdOffset, factionIdA);
    if (factionB) Memory::Read(reinterpret_cast<uintptr_t>(factionB) + factionIdOffset, factionIdB);

    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_FactionRelation);
    MsgFactionRelation msg{};
    msg.factionIdA = factionIdA;
    msg.factionIdB = factionIdB;
    msg.relation = relation;
    msg.causerEntityId = 0;
    writer.WriteRaw(&msg, sizeof(msg));
    core.GetClient().SendReliable(writer.Data(), writer.Size());
}

// ── Install / Uninstall ──

bool Install() {
    // ⚠ HOOK DESHABILITADO (RE de bytes Steam 1.0.68, 2026-06-18) ──────────────────────────
    // El símbolo "FactionRelation" resuelve a RVA 0x872E00, que NO es el setter de relaciones
    // de facción: es un LOGGER (función de traza). Hookearlo como si modificara relaciones era
    // un anclaje INCORRECTO — nunca interceptó cambios de hostilidad reales. Además el handler
    // enviaba C2S_FactionRelation con faction.id == -1 (offset no resuelto, ver FactionOffsets),
    // o sea, ids basura.
    //
    // El setter REAL de relaciones es addRelation (RVA 0x6B2EA0, FactionRelations vtbl+0x20) y
    // el getOrCreate getRelationEntry (RVA 0x6B4C60, vtbl+0x50). La causa del combate co-op
    // congelado NO era un cambio de relación que hubiera que sincronizar, sino que las facciones
    // "Player N" clonadas por ModGen nacen con el mapa de relaciones VACÍO (neutral con todos).
    // Eso lo arregla el FIX-HOSTILITY en core.cpp (poblar el map en el hilo de lógica), no este
    // hook. Por eso NO instalamos nada aquí: instalar un hook sobre un logger es inútil y arriesga
    // un detour innecesario en un hot path de traza.
    //
    // El código de Hook_FactionRelation / SEH_FactionRelation se conserva (no se borra) por si en
    // el futuro se localiza el setter REAL y se quiere sincronizar cambios de relación por red.
    // Para reactivarlo: re-anclar el patrón al RVA correcto (addRelation 0x6B2EA0) y descomentar.
    spdlog::info("faction_hooks: Install() NO-OP — 'FactionRelation' (0x872E00) es un LOGGER, no "
                 "el setter de relaciones. La hostilidad la arregla el FIX-HOSTILITY en core.cpp.");
    (void)&Hook_FactionRelation;  // silencia 'función sin usar' mientras el hook esté desactivado
    return true;  // true: no es un fallo, es una decisión de diseño (no hay nada que instalar)
}

void Uninstall() {
    auto& hooks = HookManager::Get();
    if (s_origFactionRelation) hooks.Remove("FactionRelation");
    s_origFactionRelation = nullptr;
}

void SetLoading(bool loading) {
    s_loading.store(loading, std::memory_order_release);
}

void SetServerSourced(bool sourced) {
    s_serverSourced.store(sourced, std::memory_order_release);
}

FactionRelationFn GetOriginal() {
    return s_origFactionRelation;
}

} // namespace kmp::faction_hooks
