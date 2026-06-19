#include "combat_hooks.h"
#include "ai_hooks.h"
#include "../core.h"
#include "../game/game_types.h"
#include "kmp/hook_manager.h"
#include "kmp/protocol.h"
#include "kmp/safe_hook.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <Windows.h>

namespace kmp::combat_hooks {

// ── Function Types ──
using CharacterDeathFn = void(__fastcall*)(void* character, void* killer);
using CharacterKOFn = void(__fastcall*)(void* character, void* attacker, int reason);
// IssueOrder/setJobTarget(character=RCX, target=RDX, immediate=R8b) — prólogo `48 8B C4`
// (mov rax,rsp). HookManager::InstallAt aplica el fix MovRaxRsp automáticamente.
// Es la función REAL que el motor llama cuando el jugador clica un objetivo (atacar,
// hablar, recoger...). Reemplaza al antiguo target "StartAttack" (0x7B2A20), que el RE
// confirmó que era "Cutting damage calc" — NO la orden del jugador, por eso el DIAG
// nunca se disparaba al atacar. Mantenemos el nombre de tipo por compatibilidad mínima.
using StartAttackFn = void(__fastcall*)(void* character, void* target, void* immediate);

// ── DIAG-PUSHORDER (Fase 4) ──
// Tasker::pushOrder(Tasker* this, RootObject* subject, int mode) — RVA 0x674300.
// Es donde la orden de ataque ENTRA en el map del Tasker del platoon
// (this = *(char+0x658 ActivePlatoon +0x98)). RE 2026-06-19 byte a byte:
//   attackTarget 0x5CB0A0 → enqueueCombatOrder 0x6744A0 (mode=4) → pushOrder 0x674300.
// Hookeándola confirmamos en RUNTIME si la orden del HOST llega a encolarse, y a QUÉ Tasker.
// Veredicto del "mismatch de Tasker" (consigna Fase 4): REFUTADO por RE — NO hay mismatch.
// El AI tick consume la cola correcta (vía AI+0x20 → selector → CharBody+0x68), no
// char+0x448+0xE8. Este DIAG sirve para CERRAR el caso en runtime: si pushOrder dispara
// con this==hostTasker tras attackTarget → la orden ENTRA; el AUTOTEST mide si se CONSUME.
using PushOrderFn = void(__fastcall*)(void* tasker, void* subject, int mode);

static CharacterDeathFn s_origCharDeath    = nullptr;
static CharacterKOFn    s_origCharKO       = nullptr;
static StartAttackFn    s_origStartAttack  = nullptr;
static PushOrderFn      s_origPushOrder    = nullptr;

// Tasker del host (char+0x658 ActivePlatoon → +0x98) publicado por el [AUTOTEST] del core
// (CombatAutotestTick) antes de disparar attackTarget. El hook DIAG-PUSHORDER lo compara con
// el `this` (Tasker) de cada inserción para saber si la orden ENTRÓ en el Tasker del host.
// 0 = aún no resuelto. Lo escribe el hilo de lógica (core), lo lee el hook (mismo hilo de juego).
std::atomic<uintptr_t> g_hostTaskerForDiag{0};

// ── DIAG-COMBAT: contadores y ring de eventos StartAttack ──
// Objetivo del diagnóstico: comprobar si, cuando el jugador hace clic para atacar,
// la función StartAttack del juego SE LLAMA o NO. El cuerpo del hook NO puede usar
// spdlog (corre dentro del naked detour MovRaxRsp, donde una asignación de heap
// corrompe el hueco de 4KB de pila). Por eso loguea con OutputDebugStringA (buffer
// de pila, sin heap) y, para el log estructurado de spdlog, difiere los datos a un
// ring buffer que se vacía desde ProcessDeferredEvents (contexto seguro de game tick).
static std::atomic<int> s_startAttackCount{0};

struct DeferredStartAttack {
    uintptr_t attacker;
    uintptr_t target;
    uintptr_t weapon;
};
static constexpr int MAX_SA_EVENTS = 128;
static DeferredStartAttack s_saRing[MAX_SA_EVENTS];
static std::atomic<int> s_saWriteIdx{0};
static std::atomic<int> s_saReadIdx{0};

static void PushStartAttack(const DeferredStartAttack& e) {
    int w = s_saWriteIdx.load(std::memory_order_relaxed);
    int n = (w + 1) % MAX_SA_EVENTS;
    if (n == s_saReadIdx.load(std::memory_order_acquire)) return; // lleno -> descartar
    s_saRing[w] = e;
    s_saWriteIdx.store(n, std::memory_order_release);
}

static bool PopStartAttack(DeferredStartAttack& out) {
    int r = s_saReadIdx.load(std::memory_order_relaxed);
    if (r == s_saWriteIdx.load(std::memory_order_acquire)) return false; // vacío
    out = s_saRing[r];
    s_saReadIdx.store((r + 1) % MAX_SA_EVENTS, std::memory_order_release);
    return true;
}

// ── DIAG-PUSHORDER: ring de inserciones de orden en el Tasker ──
// Capturamos (tasker=this, subject, mode) de cada pushOrder. mode==4 = orden de combate
// (la que produce attackTarget). El log diferido marca si tasker==hostTasker (la orden del
// HOST entró en su Tasker) — evidencia directa de que el encolado FUNCIONA.
static std::atomic<int> s_pushOrderCount{0};
struct DeferredPushOrder {
    uintptr_t tasker;
    uintptr_t subject;
    int       mode;
    int       isHost;   // 1 si tasker == g_hostTaskerForDiag en el momento de la inserción
};
static constexpr int MAX_PO_EVENTS = 128;
static DeferredPushOrder s_poRing[MAX_PO_EVENTS];
static std::atomic<int> s_poWriteIdx{0};
static std::atomic<int> s_poReadIdx{0};

static void PushPushOrder(const DeferredPushOrder& e) {
    int w = s_poWriteIdx.load(std::memory_order_relaxed);
    int n = (w + 1) % MAX_PO_EVENTS;
    if (n == s_poReadIdx.load(std::memory_order_acquire)) return; // lleno -> descartar
    s_poRing[w] = e;
    s_poWriteIdx.store(n, std::memory_order_release);
}

static bool PopPushOrder(DeferredPushOrder& out) {
    int r = s_poReadIdx.load(std::memory_order_relaxed);
    if (r == s_poWriteIdx.load(std::memory_order_acquire)) return false; // vacío
    out = s_poRing[r];
    s_poReadIdx.store((r + 1) % MAX_PO_EVENTS, std::memory_order_release);
    return true;
}

// ── Hook Health ──
static HookHealth s_deathHealth{"CharacterDeath"};
static HookHealth s_koHealth{"CharacterKO"};
static HookHealth s_startAttackHealth{"StartAttack"};
static HookHealth s_pushOrderHealth{"PushOrder"};

// ── Diagnostic Counters ──
static std::atomic<int> s_deathCount{0};
static std::atomic<int> s_koCount{0};

// ── Echo suppression flags ──
// Set by packet_handler before calling native CharacterDeath/CharacterKO from
// server-sourced events (S2C_CombatDeath, S2C_CombatKO). When set, the hook
// skips pushing to the deferred queue, preventing C2S→S2C→C2S echo loops.
static std::atomic<bool> s_serverSourcedDeath{false};
static std::atomic<bool> s_serverSourcedKO{false};

// ═══════════════════════════════════════════════════════════════════════════
//  DEFERRED EVENT QUEUE
//
//  Combat hooks fire inside MovRaxRsp naked detours where:
//  - spdlog calls allocate from heap → corrupt the 4KB stack gap
//  - SendReliable acquires ENet mutex → deadlock if game thread holds it
//  - PacketWriter has a destructor → stack unwinding crashes in detour context
//  - CharacterAccessor reads game memory → AV in wrong stack context
//
//  SOLUTION: The hook body does ONLY the minimum (call original + capture IDs
//  into a lock-free queue), then ProcessDeferredEvents() runs from the safe
//  OnGameTick context to do logging, packet building, and network sends.
// ═══════════════════════════════════════════════════════════════════════════

enum class CombatEventType : uint8_t { Death, KO };

struct DeferredCombatEvent {
    CombatEventType type;
    uint32_t entityId;
    uint32_t otherId;   // killer for death, attacker for KO
    uint8_t  reason;    // KO reason
};

static constexpr int MAX_DEFERRED_EVENTS = 256;
static DeferredCombatEvent s_eventRing[MAX_DEFERRED_EVENTS];
static std::atomic<int> s_eventWriteIdx{0};
static std::atomic<int> s_eventReadIdx{0};
static std::atomic<int> s_dropCount{0}; // Tracks events dropped due to full buffer

// Lock-free single-producer (hook) single-consumer (game tick) ring buffer.
// Safe because: hooks run on game thread (single producer), ProcessDeferredEvents
// runs on game thread (single consumer), and both are the SAME thread.
static bool PushEvent(const DeferredCombatEvent& evt) {
    int writeIdx = s_eventWriteIdx.load(std::memory_order_relaxed);
    int nextIdx = (writeIdx + 1) % MAX_DEFERRED_EVENTS;
    if (nextIdx == s_eventReadIdx.load(std::memory_order_acquire)) {
        s_dropCount.fetch_add(1, std::memory_order_relaxed);
        return false; // Full — drop event (better than crash)
    }
    s_eventRing[writeIdx] = evt;
    s_eventWriteIdx.store(nextIdx, std::memory_order_release);
    return true;
}

static bool PopEvent(DeferredCombatEvent& out) {
    int readIdx = s_eventReadIdx.load(std::memory_order_relaxed);
    if (readIdx == s_eventWriteIdx.load(std::memory_order_acquire)) {
        return false; // Empty
    }
    out = s_eventRing[readIdx];
    s_eventReadIdx.store((readIdx + 1) % MAX_DEFERRED_EVENTS, std::memory_order_release);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  HOOK BODIES — MINIMAL WORK ONLY
//  No spdlog. No PacketWriter. No SendReliable. No CharacterAccessor.
//  Just: call original + push event IDs to ring buffer.
// ═══════════════════════════════════════════════════════════════════════════

static void __fastcall Hook_CharacterDeath(void* character, void* killer) {
    s_deathCount.fetch_add(1, std::memory_order_relaxed);

    // Call original FIRST (SEH-protected) — game death logic must always run
    SafeCall_Void_PtrPtr(reinterpret_cast<void*>(s_origCharDeath),
                          character, killer, &s_deathHealth);

    // Skip if this death was triggered by the packet handler applying a server
    // event (S2C_CombatDeath). Without this, receiving a remote death would
    // push a new C2S_CombatDeath → server re-broadcasts → infinite echo.
    if (s_serverSourcedDeath.load(std::memory_order_acquire)) return;

    // Capture entity IDs (cheap pointer lookups, no allocation)
    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    EntityID entityId = core.GetEntityRegistry().GetNetId(character);
    if (entityId == INVALID_ENTITY) return;

    EntityID killerId = killer ? core.GetEntityRegistry().GetNetId(killer) : INVALID_ENTITY;

    // Defer all heavy work (logging, packet send) to ProcessDeferredEvents
    DeferredCombatEvent evt{};
    evt.type = CombatEventType::Death;
    evt.entityId = entityId;
    evt.otherId = killerId;
    PushEvent(evt);
}

static void __fastcall Hook_CharacterKO(void* character, void* attacker, int reason) {
    s_koCount.fetch_add(1, std::memory_order_relaxed);

    // Call original FIRST (SEH-protected) — game KO logic must always run
    SafeCall_Void_PtrPtrI(reinterpret_cast<void*>(s_origCharKO),
                           character, attacker, reason, &s_koHealth);

    // Skip if this KO was triggered by the packet handler applying a server
    // event (S2C_CombatKO). Prevents infinite echo loops.
    if (s_serverSourcedKO.load(std::memory_order_acquire)) return;

    // Capture entity IDs (cheap pointer lookups, no allocation)
    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    EntityID entityId = core.GetEntityRegistry().GetNetId(character);
    if (entityId == INVALID_ENTITY) return;

    EntityID attackerId = attacker ? core.GetEntityRegistry().GetNetId(attacker) : INVALID_ENTITY;

    // Defer all heavy work to ProcessDeferredEvents
    DeferredCombatEvent evt{};
    evt.type = CombatEventType::KO;
    evt.entityId = entityId;
    evt.otherId = attackerId;
    evt.reason = static_cast<uint8_t>(reason);
    PushEvent(evt);
}

// ═══════════════════════════════════════════════════════════════════════════
//  DIAG-COMBAT: Hook de diagnóstico en StartAttack (SOLO LOG, NO modifica nada)
//
//  StartAttack es la función que el juego invoca cuando un personaje INICIA un
//  ataque contra un objetivo. Hookeándola sabemos si la orden de ataque del
//  jugador (clic sobre un NPC) LLEGA al motor:
//    - Si vemos "[DIAG-COMBAT] StartAttack called" al atacar → la orden llega,
//      el problema está más abajo (cálculo de daño / ApplyDamage no hookeado).
//    - Si NO vemos NADA al atacar → la orden se pierde ANTES (ruta de órdenes,
//      IA, o input). El combate se rompe en la capa de comando, no de daño.
//
//  IMPORTANTE: este hook NO altera el combate. Llama al original tal cual y solo
//  registra. Cuerpo mínimo (sin spdlog/heap): OutputDebugStringA + ring buffer.
// ═══════════════════════════════════════════════════════════════════════════
static void __fastcall Hook_StartAttack(void* character, void* target, void* immediate) {
    int n = s_startAttackCount.fetch_add(1, std::memory_order_relaxed) + 1;

    // Log inmediato y barato (buffer de pila, sin heap) — visible en DebugView.
    // Throttle: primeros 30 y luego 1 de cada 20 para no spamear.
    // Ahora hookea IssueOrder (orden real del jugador): si vemos "[DIAG-COMBAT] IssueOrder
    // called" al clicar un enemigo → la orden SÍ llega al motor (el problema está abajo,
    // en el AI tick que ejecuta el Job). Si NO se dispara al atacar → la orden se pierde
    // en input/UI antes de llegar al motor.
    if (n <= 30 || n % 20 == 0) {
        char dbg[160];
        sprintf_s(dbg, "[DIAG-COMBAT] IssueOrder #%d called: char=0x%llX target=0x%llX immediate=%llu\n",
                  n,
                  (unsigned long long)reinterpret_cast<uintptr_t>(character),
                  (unsigned long long)reinterpret_cast<uintptr_t>(target),
                  (unsigned long long)(reinterpret_cast<uintptr_t>(immediate) & 0xFF));
        OutputDebugStringA(dbg);
    }

    // Difiere los punteros al ring para log estructurado (spdlog) desde game tick.
    DeferredStartAttack e{};
    e.attacker = reinterpret_cast<uintptr_t>(character);
    e.target   = reinterpret_cast<uintptr_t>(target);
    e.weapon   = reinterpret_cast<uintptr_t>(immediate);  // reutiliza el campo: flag immediate
    PushStartAttack(e);

    // Llama al original SIN modificar — la orden debe ejecutarse exactamente igual.
    SafeCall_Void_PtrPtrPtr(reinterpret_cast<void*>(s_origStartAttack),
                            character, target, immediate, &s_startAttackHealth);
}

// ═══════════════════════════════════════════════════════════════════════════
//  DIAG-PUSHORDER: Hook de diagnóstico en Tasker::pushOrder 0x674300 (Fase 4)
//
//  pushOrder es donde la orden de combate ENTRA en el map del Tasker del platoon.
//  Prólogo limpio (48 85 D2 0F 84 ...), NO mov rax,rsp → NO necesita el fix MovRaxRsp.
//  Cuerpo mínimo: registra (this=Tasker, subject, mode) y marca si this==hostTasker
//  (publicado por el AUTOTEST). Llama al original SIN modificar. Confirma en runtime que
//  la orden del host llega a encolarse y a QUÉ Tasker — cierra el "mismatch de Tasker".
// ═══════════════════════════════════════════════════════════════════════════
static void __fastcall Hook_PushOrder(void* tasker, void* subject, int mode) {
    int n = s_pushOrderCount.fetch_add(1, std::memory_order_relaxed) + 1;

    uintptr_t taskerU = reinterpret_cast<uintptr_t>(tasker);
    uintptr_t hostTasker = g_hostTaskerForDiag.load(std::memory_order_acquire);
    int isHost = (hostTasker != 0 && taskerU == hostTasker) ? 1 : 0;

    // Log barato (sin heap) para órdenes de combate (mode==4) o del host. Throttle suave.
    if (isHost || mode == 4 || n <= 20 || n % 100 == 0) {
        char dbg[180];
        sprintf_s(dbg, "[DIAG-PUSHORDER] #%d tasker=0x%llX subject=0x%llX mode=%d isHost=%d\n",
                  n,
                  (unsigned long long)taskerU,
                  (unsigned long long)reinterpret_cast<uintptr_t>(subject),
                  mode, isHost);
        OutputDebugStringA(dbg);
    }

    // Difiere para log estructurado (spdlog) desde el game tick.
    DeferredPushOrder e{};
    e.tasker  = taskerU;
    e.subject = reinterpret_cast<uintptr_t>(subject);
    e.mode    = mode;
    e.isHost  = isHost;
    PushPushOrder(e);

    // Llama al original SIN modificar — la orden debe insertarse exactamente igual.
    SafeCall_Void_PtrPtrI(reinterpret_cast<void*>(s_origPushOrder),
                          tasker, subject, mode, &s_pushOrderHealth);
}

// ── SEH wrapper for reading health (no C++ objects allowed in __try) ──
static float SEH_ReadChestHealth(void* gameObj) {
    __try {
        game::CharacterAccessor accessor(gameObj);
        return accessor.GetHealth(BodyPart::Chest);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -100.f; // Assume dead on read failure
    }
}

// ── SEH wrapper for reading all 7 limb health values ──
static bool SEH_ReadAllLimbHealth(void* gameObj, float outHealth[7]) {
    __try {
        game::CharacterAccessor accessor(gameObj);
        for (int i = 0; i < static_cast<int>(BodyPart::Count); i++) {
            outHealth[i] = accessor.GetHealth(static_cast<BodyPart>(i));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        for (int i = 0; i < 7; i++) outHealth[i] = -100.f;
        return false;
    }
}

// ── Helper: send C2S_LimbHealth for an entity after a combat event ──
static void SendLimbHealthUpdate(EntityID entityId, void* gameObj) {
    if (!gameObj) return;
    float limbHealth[7];
    if (!SEH_ReadAllLimbHealth(gameObj, limbHealth)) return;

    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_LimbHealth);
    writer.WriteU32(entityId);
    for (int i = 0; i < 7; i++) {
        writer.WriteF32(limbHealth[i]);
    }
    Core::Get().GetClient().SendReliable(writer.Data(), writer.Size());
}

// ═══════════════════════════════════════════════════════════════════════════
//  DEFERRED PROCESSING — called from Core::OnGameTick (safe context)
//  Here we can safely: log, build packets, send via ENet, read game memory.
// ═══════════════════════════════════════════════════════════════════════════

void ProcessDeferredEvents() {
    auto& core = Core::Get();
    if (!core.IsConnected()) {
        // Drain queue without processing if disconnected
        DeferredCombatEvent discard;
        while (PopEvent(discard)) {}
        // DIAG-COMBAT: aun desconectado, logueamos StartAttack (es diagnóstico de
        // combate single-player del host; no requiere estar conectado). El host
        // puede atacar sin conexión y queremos saber si StartAttack se dispara.
        DeferredStartAttack sa;
        int saN = 0;
        while (PopStartAttack(sa) && saN < 32) {
            saN++;
            spdlog::info("[DIAG-COMBAT] IssueOrder [deferred,offline] char=0x{:X} "
                         "target=0x{:X} immediate={}", sa.attacker, sa.target, sa.weapon & 0xFF);
        }
        // DIAG-PUSHORDER también offline: el host puede atacar sin conexión y queremos saber
        // si la orden ENTRA en su Tasker (el AUTOTEST corre offline igual).
        DeferredPushOrder po;
        int poN = 0;
        while (PopPushOrder(po) && poN < 32) {
            poN++;
            spdlog::info("[DIAG-PUSHORDER] [offline] tasker=0x{:X} subject=0x{:X} mode={} isHost={}"
                         "{}", po.tasker, po.subject, po.mode, po.isHost,
                         (po.isHost && po.mode == 4)
                             ? "  <== ORDEN DE COMBATE DEL HOST ENCOLADA (el encolado FUNCIONA)"
                             : "");
        }
        return;
    }

    // Report and reset drop counter (can't log from hook context, so we log here)
    int dropped = s_dropCount.exchange(0, std::memory_order_relaxed);
    if (dropped > 0) {
        spdlog::warn("combat_hooks: {} combat events DROPPED (ring buffer was full)", dropped);
    }

    // ── DIAG-COMBAT: vaciado del ring de StartAttack (log estructurado seguro) ──
    // Aquí (game tick) sí podemos usar spdlog y consultar el registro de entidades.
    // Resolvemos netId del atacante y del objetivo para entender QUIÉN ataca a QUIÉN
    // y si el motor reconoce esos personajes como entidades de red registradas.
    {
        DeferredStartAttack sa;
        int saProcessed = 0;
        while (PopStartAttack(sa) && saProcessed < 32) {
            saProcessed++;
            EntityID atkId = core.GetEntityRegistry().GetNetId(reinterpret_cast<void*>(sa.attacker));
            EntityID tgtId = core.GetEntityRegistry().GetNetId(reinterpret_cast<void*>(sa.target));
            spdlog::info("[DIAG-COMBAT] IssueOrder [deferred] char=0x{:X}(net={}) "
                         "target=0x{:X}(net={}) immediate={}",
                         sa.attacker, (atkId == INVALID_ENTITY ? -1 : (int)atkId),
                         sa.target,   (tgtId == INVALID_ENTITY ? -1 : (int)tgtId),
                         sa.weapon & 0xFF);
        }
    }

    // ── DIAG-PUSHORDER: vaciado del ring (log estructurado seguro) ──
    // mode==4 = orden de combate (la que genera attackTarget). isHost=1 → la orden se insertó en
    // el Tasker del HOST. Si vemos `mode=4 isHost=1` tras el disparo del AUTOTEST → la orden de
    // ataque del host ENTRA en su cola correctamente (encolado OK). Que LUEGO ataque o no lo
    // mide el [AUTOTEST] (CharBody+0x68 / currentTarget / amIdle). Juntos cierran la Fase 4:
    //   pushOrder isHost+mode4 SÍ  +  AUTOTEST amIdle=1  → encola pero el AI tick NO lo consume.
    //   pushOrder isHost+mode4 NO              → la orden ni siquiera llega (gate isAlly / facción).
    {
        DeferredPushOrder po;
        int poProcessed = 0;
        while (PopPushOrder(po) && poProcessed < 32) {
            poProcessed++;
            spdlog::info("[DIAG-PUSHORDER] tasker=0x{:X} subject=0x{:X} mode={} isHost={}{}",
                         po.tasker, po.subject, po.mode, po.isHost,
                         (po.isHost && po.mode == 4)
                             ? "  <== ORDEN DE COMBATE DEL HOST ENCOLADA (el encolado FUNCIONA)"
                             : "");
        }
    }

    DeferredCombatEvent evt;
    int processed = 0;
    while (PopEvent(evt) && processed < 64) { // Cap per tick to avoid stalls
        processed++;

        // Send events for ANY registered entity (not just owned).
        // Cross-player combat: when player A kills player B's character,
        // the death fires on A's machine and must be reported to the server.
        // Echo suppression is handled by s_serverSourcedDeath/KO flags in
        // the hook body — events that arrive here are genuine local combat.
        auto infoCopy = core.GetEntityRegistry().GetInfo(evt.entityId);
        if (!infoCopy) continue;

        if (evt.type == CombatEventType::Death) {
            spdlog::info("combat_hooks: [deferred] Death — entity {} killer {}",
                         evt.entityId, evt.otherId);

            PacketWriter writer;
            writer.WriteHeader(MessageType::C2S_CombatDeath);
            writer.WriteU32(evt.entityId);
            writer.WriteU32(evt.otherId);
            core.GetClient().SendReliable(writer.Data(), writer.Size());

            // Piggyback limb health snapshot after death event
            void* deathObj = core.GetEntityRegistry().GetGameObject(evt.entityId);
            SendLimbHealthUpdate(evt.entityId, deathObj);

        } else if (evt.type == CombatEventType::KO) {
            spdlog::info("combat_hooks: [deferred] KO — entity {} attacker {} reason {}",
                         evt.entityId, evt.otherId, evt.reason);

            PacketWriter writer;
            writer.WriteHeader(MessageType::C2S_CombatKO);
            writer.WriteU32(evt.entityId);
            writer.WriteU32(evt.otherId);
            writer.WriteU8(evt.reason);

            // Read health safely from game tick context (not hook context)
            void* gameObj = core.GetEntityRegistry().GetGameObject(evt.entityId);
            float chestHealth = gameObj ? SEH_ReadChestHealth(gameObj) : -100.f;
            writer.WriteF32(chestHealth);
            core.GetClient().SendReliable(writer.Data(), writer.Size());

            // Piggyback limb health snapshot after KO event
            SendLimbHealthUpdate(evt.entityId, gameObj);
        }
    }
}

// ── Install/Uninstall ──

bool Install() {
    auto& core = Core::Get();
    auto& hookMgr = HookManager::Get();
    auto& funcs = core.GetGameFunctions();

    // ═══ DO NOT HOOK ApplyDamage ═══
    // ApplyDamage (0x7A33A0) uses `mov rax, rsp` prologue and fires hundreds
    // of times per combat tick. The MovRaxRsp wrapper's global RSP slots corrupt
    // under rapid-fire calls → deterministic crash on "attack unprovoked".
    // Combat damage sync uses death/KO hooks + health polling instead.
    if (funcs.ApplyDamage) {
        spdlog::info("combat_hooks: ApplyDamage at 0x{:X} — NOT hooked (mov rax,rsp crash risk)",
                     reinterpret_cast<uintptr_t>(funcs.ApplyDamage));
    }

    if (funcs.CharacterDeath) {
        hookMgr.InstallAt("CharacterDeath",
                          reinterpret_cast<uintptr_t>(funcs.CharacterDeath),
                          &Hook_CharacterDeath, &s_origCharDeath);
    }

    if (funcs.CharacterKO) {
        hookMgr.InstallAt("CharacterKO",
                          reinterpret_cast<uintptr_t>(funcs.CharacterKO),
                          &Hook_CharacterKO, &s_origCharKO);
    }

    // ═══ DIAG-COMBAT en IssueOrder: DESHABILITADO ═══
    // 0x722EF0 (que funcs.IssueOrder resuelve) NO es la orden del jugador: el RE de bytes
    // (doble verificación 2026-06-18) demostró que construye un MyGUI::UString → es UI/GUI,
    // NO el dispatcher de Jobs. Hookearla con el fix MovRaxRsp no diagnostica nada útil y
    // añade riesgo gratuito sobre una función de UI de alta frecuencia. Se DESHABILITA hasta
    // resolver la RVA real de la orden (Tasker/GOAPTaskMgr → Task_MeleeAttack/Task_GetUp...).
    // El cuerpo Hook_StartAttack se conserva por si se re-apunta a la función correcta.
    spdlog::warn("combat_hooks: [DIAG] hook de IssueOrder DESHABILITADO — 0x{:X} es UI/MyGUI, "
                 "NO la orden del jugador (refutado por RE). La orden real entra por "
                 "Tasker/GOAPTaskMgr (RVA sin resolver).",
                 reinterpret_cast<uintptr_t>(funcs.IssueOrder));

    // ═══ DIAG-PUSHORDER en Tasker::pushOrder 0x674300 ═══
    // ESTA SÍ es la función de encolado real (RE byte a byte 2026-06-19, AOB único, prólogo
    // limpio sin mov rax,rsp). Confirma en runtime si la orden de ataque del HOST entra en su
    // Tasker (char+0x658 ActivePlatoon +0x98). Se instala DESHABILITADA y se activa al cargar
    // el juego (Core::OnGameLoaded → HookManager::Enable("PushOrder")), igual que el resto de
    // hooks de combate. SOLO registra (no muta la orden).
    if (funcs.PushOrder) {
        hookMgr.InstallAt("PushOrder",
                          reinterpret_cast<uintptr_t>(funcs.PushOrder),
                          &Hook_PushOrder, &s_origPushOrder);
        HookManager::Get().Disable("PushOrder"); // arranca OFF; se activa al cargar el juego
        spdlog::info("combat_hooks: [DIAG-PUSHORDER] hook instalado en Tasker::pushOrder 0x{:X} "
                     "(arranca deshabilitado)", reinterpret_cast<uintptr_t>(funcs.PushOrder));
    } else {
        spdlog::warn("combat_hooks: [DIAG-PUSHORDER] funcs.PushOrder no resuelto — DIAG no disponible");
    }

    spdlog::info("combat_hooks: Installed (damage=SKIPPED, death={}, ko={}, issueOrder_DIAG=DISABLED, "
                 "pushOrder_DIAG={})",
                 funcs.CharacterDeath != nullptr, funcs.CharacterKO != nullptr,
                 funcs.PushOrder != nullptr);
    return true;
}

void Uninstall() {
    HookManager::Get().Remove("CharacterDeath");
    HookManager::Get().Remove("CharacterKO");
    HookManager::Get().Remove("StartAttack"); // DIAG-COMBAT
    HookManager::Get().Remove("PushOrder");   // DIAG-PUSHORDER
    spdlog::info("combat_hooks: Uninstalled");
}

void SetServerSourcedDeath(bool active) {
    s_serverSourcedDeath.store(active, std::memory_order_release);
}

void SetServerSourcedKO(bool active) {
    s_serverSourcedKO.store(active, std::memory_order_release);
}

} // namespace kmp::combat_hooks
