// ES: Implementación de los hooks de relaciones de facción. Contiene el detour
//     Hook_FactionRelation (llama al original protegido con SEH y, si hay conexión y no
//     estamos cargando ni aplicando datos del servidor, envía C2S_FactionRelation con los
//     ids de ambas facciones). DESACTIVADO desde 2026-06-18: la RVA 0x872E00 es un logger,
//     no el setter real (addRelation, RVA 0x6B2EA0). El código se conserva para reactivarlo.
// EN: Implementation of the faction relation hooks. Contains the Hook_FactionRelation
//     detour (calls the original under SEH and, when connected and neither loading nor
//     applying server data, sends C2S_FactionRelation with both faction ids). DISABLED
//     since 2026-06-18: RVA 0x872E00 is a logger, not the real setter (addRelation,
//     RVA 0x6B2EA0). The code is kept so it can be re-enabled.
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

// ES: Firma de la función del juego: (facción A, facción B, valor de relación).
// EN: Game function signature: (faction A, faction B, relation value).
// ── Function typedefs ──
using FactionRelationFn = void(__fastcall*)(void* factionA, void* factionB, float relation);

// ES: Estado: trampolín al original (nullptr si no hay hook), contador de llamadas para
//     el log y las dos banderas de supresión (carga de partida / cambio venido del servidor).
// EN: State: trampoline to the original (nullptr when unhooked), call counter for logging
//     and the two suppression flags (save load / server-sourced change).
// ── State ──
static FactionRelationFn s_origFactionRelation = nullptr;
static int s_relationChangeCount = 0;
static std::atomic<bool> s_loading{false};
static std::atomic<bool> s_serverSourced{false};

// ES: Envoltorio SEH: llama al original capturando excepciones de acceso a memoria.
//     Devuelve false si la llamada crasheó.
// EN: SEH wrapper: calls the original catching memory access exceptions.
//     Returns false if the call crashed.
// ── SEH wrapper ──

static bool SEH_FactionRelation(void* factionA, void* factionB, float relation) {
    __try {
        s_origFactionRelation(factionA, factionB, relation);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Detour de FactionRelation (no instalado). Primero ejecuta el original; después,
//     salvo que estemos cargando o aplicando un cambio del servidor, lee el id de cada
//     facción (offset faction.id de la tabla de offsets) y envía C2S_FactionRelation
//     fiable. Corre en el hilo del juego que llame a la función.
// EN: FactionRelation detour (not installed). Runs the original first; then, unless we
//     are loading or applying a server change, reads each faction's id (faction.id
//     offset from the offsets table) and sends a reliable C2S_FactionRelation. Runs on
//     whichever game thread calls the function.
// ── Hooks ──

static void __fastcall Hook_FactionRelation(void* factionA, void* factionB, float relation) {
    s_relationChangeCount++;

    if (!SEH_FactionRelation(factionA, factionB, relation)) {
        spdlog::error("faction_hooks: FactionRelation crashed");
        return;
    }

    // ES: Guardas anti-bucle: no reenviar durante la carga ni cambios que vienen del servidor.
    // EN: Anti-loop guards: do not resend during load or for server-sourced changes.
    if (s_loading.load(std::memory_order_acquire) || s_serverSourced.load(std::memory_order_acquire)) return;

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    spdlog::info("faction_hooks: FactionRelation #{} (A=0x{:X}, B=0x{:X}, rel={:.2f})",
                  s_relationChangeCount, (uintptr_t)factionA, (uintptr_t)factionB, relation);

    // ES: Leer el id numérico de cada facción (campo en facción+faction.id). Si el offset
    //     no está resuelto (-1) no se envía nada para no mandar ids basura.
    // EN: Read each faction's numeric id (field at faction+faction.id). If the offset is
    //     unresolved (-1) nothing is sent, to avoid garbage ids.
    uint32_t factionIdA = 0, factionIdB = 0;
    const int factionIdOffset = game::GetOffsets().faction.id;
    if (factionIdOffset < 0) {
        spdlog::warn("faction_hooks: faction.id offset not resolved (-1), skipping relation packet");
        return;
    }
    if (factionA) Memory::Read(reinterpret_cast<uintptr_t>(factionA) + factionIdOffset, factionIdA);
    if (factionB) Memory::Read(reinterpret_cast<uintptr_t>(factionB) + factionIdOffset, factionIdB);

    // ES: Construir y enviar el paquete C2S_FactionRelation (canal fiable).
    // EN: Build and send the C2S_FactionRelation packet (reliable channel).
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

// ES: Install() deliberadamente vacío (explicación en el bloque siguiente).
// EN: Install() is deliberately empty. English summary of the block below: HOOK DISABLED
//     (byte-level RE of Steam 1.0.68, 2026-06-18). The "FactionRelation" symbol resolves
//     to RVA 0x872E00, which is NOT the faction relation setter but a LOGGER (trace
//     function); hooking it never intercepted real hostility changes, and the handler sent
//     C2S_FactionRelation with faction.id == -1 (unresolved offset), i.e. garbage ids.
//     The REAL setter is addRelation (RVA 0x6B2EA0, FactionRelations vtbl+0x20) and the
//     getOrCreate is getRelationEntry (RVA 0x6B4C60, vtbl+0x50). The frozen co-op combat
//     was caused by "Player N" factions cloned by ModGen being born with an EMPTY relation
//     map (neutral to everyone); FIX-HOSTILITY in core.cpp fixes that on the logic thread.
//     The hook code is kept; to re-enable, re-anchor the pattern to addRelation 0x6B2EA0.
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

// ES: Quita el hook si existe y olvida el trampolín.
// EN: Removes the hook if present and forgets the trampoline.
void Uninstall() {
    auto& hooks = HookManager::Get();
    if (s_origFactionRelation) hooks.Remove("FactionRelation");
    s_origFactionRelation = nullptr;
}

// ES: Activa/desactiva la supresión por carga de partida.
// EN: Enables/disables the save-load suppression.
void SetLoading(bool loading) {
    s_loading.store(loading, std::memory_order_release);
}

// ES: Activa/desactiva la supresión por cambio venido del servidor.
// EN: Enables/disables the server-sourced suppression.
void SetServerSourced(bool sourced) {
    s_serverSourced.store(sourced, std::memory_order_release);
}

// ES: Devuelve el trampolín al original (nullptr mientras el hook esté desactivado).
// EN: Returns the trampoline to the original (nullptr while the hook is disabled).
FactionRelationFn GetOriginal() {
    return s_origFactionRelation;
}

} // namespace kmp::faction_hooks
