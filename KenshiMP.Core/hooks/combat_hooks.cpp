#include "combat_hooks.h"
#include "ai_hooks.h"
#include "../core.h"
#include "../game/game_types.h"
#include "kmp/hook_manager.h"
#include "kmp/memory.h"
#include "kmp/protocol.h"
#include "kmp/safe_hook.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstring>   // memcpy / strcmp — FIX-UAF-COMBATTARGET (recorrido de secciones PE)
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

// ── DIAG-ADDORDER-BACKEND ──
// Character::addOrder backend "normal/replace" — RVA 0x5D1940 (thunk 0x274A26, llamado por
// addOrder 0x5D20D0). Es el VALIDADOR que traga órdenes: valida ANTES de encolar y, si el
// chequeo de brazos falla, muestra un globo nativo ("My arm is broken!" / "I can't carry
// anyone with this arm.") y devuelve TRUE ("manejada") SIN encolar nada — la orden de
// atacar/comer/cargar desaparece en silencio. Si pasa, devuelve FALSE y la orden sigue
// (se construye el Task y se inserta en la cola en 0x508380).
// Firma VERIFICADA por RE estática 2026-07-12 (función 0x5D1940..0x5D20C3, 0x783 bytes):
// solo usa rcx/edx/r8 (0 usos de r9 y 0 lecturas de args de pila del caller), retorno bool
// en al (mov al,1 en las 2 ramas de aborto; xor al,al en la salida normal). Prólogo limpio
// 40 55 56 57 (SIN mov rax,rsp) → MinHook normal, sin fix MovRaxRsp.
using AddOrderBackendFn = bool(__fastcall*)(void* character, int taskType, void* subject);

// ── DIAG-COMBATSEED ──
// CombatClass::update(float) — RVA 0x60D650, mangled ?update@CombatClass@@UEAAXM@Z.
// Virtual, retorno void, 1 argumento float (dt) → this en rcx, dt en xmm1. Prólogo LIMPIO
// (40 53 48 83 EC 20 48 8B D9, sin mov rax,rsp) → MinHook normal, sin el fix MovRaxRsp, así que
// el hook body PUEDE leer memoria del juego con Memory::Read (SEH) igual que Hook_AddOrderBackend.
using CombatClassUpdateFn = void(__fastcall*)(void* combatClass, float dt);

static CharacterDeathFn s_origCharDeath    = nullptr;
static CharacterKOFn    s_origCharKO       = nullptr;
static StartAttackFn    s_origStartAttack  = nullptr;
static PushOrderFn      s_origPushOrder    = nullptr;
static AddOrderBackendFn s_origAddOrderBackend = nullptr;
static CombatClassUpdateFn s_origCombatClassUpdate = nullptr;

// Tasker del host (char+0x658 ActivePlatoon → +0x98) publicado por el [AUTOTEST] del core
// (CombatAutotestTick) antes de disparar attackTarget. El hook DIAG-PUSHORDER lo compara con
// el `this` (Tasker) de cada inserción para saber si la orden ENTRÓ en el Tasker del host.
// 0 = aún no resuelto. Lo escribe el hilo de lógica (core), lo lee el hook (mismo hilo de juego).
std::atomic<uintptr_t> g_hostTaskerForDiag{0};

// [DIAG-COMBATSEED] CombatClass y AI del host, publicados cada tick por el hilo de lógica
// (PublishHostCombatDiag, llamada desde ProcessDeferredEvents). El hook de CombatClass::update
// los lee para (a) filtrar SOLO los ticks del CombatClass del host y (b) leer AI+0x28.
std::atomic<uintptr_t> g_hostCombatClassForDiag{0};
std::atomic<uintptr_t> g_hostAiForDiag{0};

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

// ── DIAG-ADDORDER-BACKEND: ring de llamadas al validador de órdenes ──
// Capturamos taskType, punteros, los bytes de estado médico que el validador consulta
// (char+0x5BD/+0x5BE = flags canUseArms; [[AI+0x318]+0x166] = byte de brazos para cargar)
// y el RETORNO del original (true = orden ABORTADA/tragada, false = orden continúa).
static std::atomic<int> s_addOrderCount{0};
struct DeferredAddOrder {
    uintptr_t character;    // rcx — Character* que recibe la orden
    uintptr_t subject;      // r8  — target/subject (Character* u otro según taskType)
    uintptr_t ai;           // *(char+0x650) — puntero AI (0 si lectura falló o null)
    uintptr_t aiSub;        // [AI+0x318] (0 si null/ilegible)
    int       taskType;     // edx
    uint8_t   arm5BD;       // byte char+0x5BD (0xFF = ilegible)
    uint8_t   arm5BE;       // byte char+0x5BE (0xFF = ilegible)
    uint8_t   carryArm;     // byte [[AI+0x318]+0x166] (0xFF = ilegible/cadena rota)
    uint8_t   ret;          // retorno del original: 1=ABORTADA (tragada), 0=OK (continúa)
    uint8_t   callOk;       // 1 si el trampoline se ejecutó sin excepción SEH
};
static constexpr int MAX_AO_EVENTS = 128;
static DeferredAddOrder s_aoRing[MAX_AO_EVENTS];
static std::atomic<int> s_aoWriteIdx{0};
static std::atomic<int> s_aoReadIdx{0};

static void PushAddOrder(const DeferredAddOrder& e) {
    int w = s_aoWriteIdx.load(std::memory_order_relaxed);
    int n = (w + 1) % MAX_AO_EVENTS;
    if (n == s_aoReadIdx.load(std::memory_order_acquire)) return; // lleno -> descartar
    s_aoRing[w] = e;
    s_aoWriteIdx.store(n, std::memory_order_release);
}

static bool PopAddOrder(DeferredAddOrder& out) {
    int r = s_aoReadIdx.load(std::memory_order_relaxed);
    if (r == s_aoWriteIdx.load(std::memory_order_acquire)) return false; // vacío
    out = s_aoRing[r];
    s_aoReadIdx.store((r + 1) % MAX_AO_EVENTS, std::memory_order_release);
    return true;
}

// ── DIAG-COMBATSEED: ring de snapshots del CombatClass del host ──
// Cada snapshot captura el estado del CombatClass del host en un tick de CombatClass::update:
// contador de percepciones (+0x200), currentTarget (+0x290), campo +0x1F0, y AI+0x28
// (AttackState inline, el target que attackTarget escribe al encolar). El objetivo es comparar
// en el log el estado "host en frío recién reclamado" vs "host tras golpear el muñeco" vs
// "host atacando con éxito" y localizar QUÉ campo cambia (para sembrarlo al reclamar el char).
struct DeferredCombatSeed {
    uintptr_t combatClass;   // this del tick (== g_hostCombatClassForDiag)
    uintptr_t f1F0;          // *(CombatClass+0x1F0)  (campo previo al array de percepciones)
    uint32_t  cnt200;        // *(CombatClass+0x200)  (contador de percepciones de combate)
    uintptr_t target290;     // *(CombatClass+0x290)  (currentTarget que produce el AI tick)
    uintptr_t ai28;          // *(AI+0x28)            (AttackState inline: target encolado)
    uint32_t  seenCount;     // nº de ticks vistos para este CombatClass del host en la sesión
    uint8_t   firstSight;    // 1 si es la 1ª vez que se ve ESTE CombatClass (cambió el puntero)
    uint8_t   readOk;        // 1 si las lecturas SEH del CombatClass no fallaron
};
static constexpr int MAX_CS_EVENTS = 128;
static DeferredCombatSeed s_csRing[MAX_CS_EVENTS];
static std::atomic<int> s_csWriteIdx{0};
static std::atomic<int> s_csReadIdx{0};

static void PushCombatSeed(const DeferredCombatSeed& e) {
    int w = s_csWriteIdx.load(std::memory_order_relaxed);
    int n = (w + 1) % MAX_CS_EVENTS;
    if (n == s_csReadIdx.load(std::memory_order_acquire)) return; // lleno -> descartar
    s_csRing[w] = e;
    s_csWriteIdx.store(n, std::memory_order_release);
}

static bool PopCombatSeed(DeferredCombatSeed& out) {
    int r = s_csReadIdx.load(std::memory_order_relaxed);
    if (r == s_csWriteIdx.load(std::memory_order_acquire)) return false; // vacío
    out = s_csRing[r];
    s_csReadIdx.store((r + 1) % MAX_CS_EVENTS, std::memory_order_release);
    return true;
}

// Estado de sesión del DIAG-COMBATSEED (mismo hilo de juego → sin locks).
// s_lastSeenHostCC diferencia "primera vez que se ve este CombatClass del host" (el puntero
// cambió, p.ej. tras el FIX-COMBATCLASS o un re-claim) de "ya se había visto".
static std::atomic<uintptr_t> s_lastSeenHostCC{0};
static std::atomic<uint32_t>  s_hostCCSeenCount{0};
static std::atomic<uint64_t>  s_lastSeedLogMs{0};   // GetTickCount64 del último log (throttle)

// ── Hook Health ──
static HookHealth s_deathHealth{"CharacterDeath"};
static HookHealth s_koHealth{"CharacterKO"};
static HookHealth s_startAttackHealth{"StartAttack"};
static HookHealth s_pushOrderHealth{"PushOrder"};
static HookHealth s_addOrderHealth{"AddOrderBackend"};
static HookHealth s_combatSeedHealth{"CombatClassUpdate"};

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

// ═══════════════════════════════════════════════════════════════════════════
//  DIAG-ADDORDER-BACKEND: Hook de diagnóstico en el validador de órdenes 0x5D1940
//
//  SOLO LECTURA/LOG — reenvía los MISMOS argumentos al original y devuelve
//  EXACTAMENTE su retorno, sin alterar el comportamiento del juego.
//
//  Qué diagnostica: el backend "normal/replace" de Character::addOrder valida la
//  orden ANTES de encolarla. Si el chequeo falla → globo nativo + return TRUE sin
//  encolar (la orden del host se TRAGA en silencio). Si pasa → return FALSE y la
//  orden sigue. Con este hook vemos, por cada orden: taskType, punteros, el estado
//  de los bytes médicos que el validador consulta, y el veredicto:
//    ret=true  → "ABORTADA (orden tragada)"  ← el bug de órdenes del host
//    ret=false → "OK (orden continúa)"
//
//  Ramas del validador (las ejecuta el ORIGINAL, aquí solo leemos para el log):
//   - taskType∈{4,5,0x10,0x15} y {0x106,0x107}: canUseArms 0x644150 lee char+0x5BD
//     y char+0x5BE (MedicalSystem inline en char+0x458, flags +0x165/+0x166).
//     Ambos a 0 → aborta con "My arm is broken!".
//   - taskType∈{0x69,0x44,0xD5,0xAA,0x94,0xE1} (acciones de brazos, incl. CARGAR):
//     lee [[AI+0x318]+0x166] con AI=*(char+0x650). Byte a 0 → aborta con
//     "I can't carry anyone with this arm.".
//
//  Lecturas SEH-safe con Memory::Read (nunca derefs ciegos: character/AI vienen del
//  juego pero un clon del mod podría tener la cadena AI incompleta). Prólogo limpio
//  → MinHook normal (sin naked detour), pero mantenemos el patrón del proyecto:
//  ODS barato en el cuerpo + spdlog diferido desde ProcessDeferredEvents.
// ═══════════════════════════════════════════════════════════════════════════
static bool __fastcall Hook_AddOrderBackend(void* character, int taskType, void* subject) {
    int n = s_addOrderCount.fetch_add(1, std::memory_order_relaxed) + 1;

    uintptr_t charU = reinterpret_cast<uintptr_t>(character);

    // ── Lecturas de diagnóstico SEH-safe (0xFF = ilegible) ANTES del original ──
    uint8_t arm5BD = 0xFF, arm5BE = 0xFF, carryArm = 0xFF;
    uintptr_t ai = 0, aiSub = 0;
    if (charU) {
        Memory::Read(charU + 0x5BD, arm5BD);            // flag brazo 1 (canUseArms)
        Memory::Read(charU + 0x5BE, arm5BE);            // flag brazo 2 (canUseArms)
        if (Memory::Read(charU + 0x650, ai) && ai) {    // AI* del Character
            if (Memory::Read(ai + 0x318, aiSub) && aiSub) {
                Memory::Read(aiSub + 0x166, carryArm);  // byte de brazos para CARGAR
            }
        }
    }

    // ── FIX-CARRY-HAND: bypass del gate para la orden de CARGA del host con "hand" corrupto ──
    // El clon del host tiene [AI+0x318] (aquí la variable 'aiSub') apuntando a una vtable
    // estática de .rdata (DENTRO del módulo del juego, memoria de solo lectura) en vez de a una
    // instancia real de heap. El gate nativo lee [aiSub+0x166] (byte constante en .rdata, vale 0
    // fijo para siempre) y aborta la orden con "I can't carry anyone with this arm.". No se puede
    // corregir el byte en caliente sin arriesgar corromper la vtable que comparten TODOS los
    // personajes.
    // RE estático (2026-07-13, escaneo completo de .text) confirma que saltarse el gate en este
    // caso concreto es SEGURO: NINGÚN código del juego usa este puntero "hand" como `this` de una
    // llamada virtual (0 vcalls sobre él en todo el binario, solo se lee como constante), y la
    // ejecución real del pickup (Character_Pick_Up_Person, RVA 0x34FBE0) nunca vuelve a leer
    // [AI+0x318]. Equivale exactamente a que el gate nativo hubiera dado la rama "brazos OK".
    // Doble discriminador (AMBOS deben cumplirse para activar el bypass):
    //   1) taskType ∈ {0x69,0x44,0xD5,0xAA,0x94,0xE1} → acción de brazos que incluye CARGAR
    //      (mismo conjunto que documenta la cabecera de esta función).
    //   2) aiSub cae DENTRO del rango [base, base+size) del propio módulo del juego → es la
    //      vtable estática de .rdata; un hand REAL de heap SIEMPRE cae FUERA de ese rango.
    // IsHost() es a nivel de PROCESO (este proceso es el host de la partida), NO filtra por
    // personaje — Hook_AddOrderBackend dispara para cualquier addOrder, incluida la IA de NPCs.
    // El discriminador REAL es (2): un hand legítimo de heap nunca cae en el rango del módulo,
    // así que IsHost()==false ya basta para no tocar nada en un cliente. Si esto SÍ dispara sobre
    // un NPC/clon remoto con el mismo bug de clonación, es inofensivo (RE confirmó 0 vcalls sobre
    // el hand corrupto en todo el binario) e incluso deseable en coop.
    if (aiSub && Core::Get().IsHost()) {
        // ¿taskType es una acción de brazos (incluye CARGAR)?
        bool isCarryTask = (taskType == 0x69 || taskType == 0x44 || taskType == 0xD5 ||
                            taskType == 0xAA || taskType == 0x94 || taskType == 0xE1);
        if (isCarryTask) {
            uintptr_t modBase = Core::Get().GetScanner().GetBase();  // base del módulo del juego
            size_t    modSize = Core::Get().GetScanner().GetSize();  // tamaño del módulo
            // 'hand' corrupto detectado: aiSub apunta dentro del propio módulo (vtable .rdata),
            // no a una instancia de heap → dejar pasar la orden como si el gate diera luz verde.
            if (modBase && aiSub >= modBase && aiSub < modBase + modSize) {
                char dbg[256];
                sprintf_s(dbg, "[FIX-CARRY-HAND] taskType=0x%X aiSub=0x%llX (hand en rango del "
                          "modulo, vtable .rdata) -> bypass gate, orden continua\n",
                          (unsigned)taskType, (unsigned long long)aiSub);
                OutputDebugStringA(dbg);
                // return false == rama "arm OK" nativa: NO reenviar al gate original (0x5D1940);
                // el caller sigue el pipeline de encolado normal y la orden de carga continúa.
                return false;
            }
        }
    }

    // ── Llama al original con los MISMOS argumentos y captura su retorno ──
    bool ret = false;
    bool callOk = SafeCall_Bool_PtrIPtr(reinterpret_cast<void*>(s_origAddOrderBackend),
                                        character, taskType, subject,
                                        &ret, &s_addOrderHealth);

    // ── Log inmediato barato (buffer de pila, sin heap) — visible en DebugView ──
    // Las órdenes TRAGADAS (ret=true) se loguean SIEMPRE (son el objetivo del DIAG);
    // el resto con throttle (primeras 50 y 1 de cada 50) — el backend también lo
    // llama la IA, no solo el jugador, y puede ser frecuente.
    if (ret || n <= 50 || n % 50 == 0) {
        char dbg[256];
        sprintf_s(dbg, "[DIAG-ADDORDER-BACKEND] #%d taskType=0x%X char=0x%llX subject=0x%llX "
                  "arm5BD=%d arm5BE=%d ai=0x%llX aiSub=0x%llX carryArm=%d ret=%s%s\n",
                  n, (unsigned)taskType,
                  (unsigned long long)charU,
                  (unsigned long long)reinterpret_cast<uintptr_t>(subject),
                  (int)arm5BD, (int)arm5BE,
                  (unsigned long long)ai, (unsigned long long)aiSub, (int)carryArm,
                  ret ? "ABORTADA (orden tragada)" : "OK (orden continua)",
                  callOk ? "" : " [TRAMPOLINE FAIL]");
        OutputDebugStringA(dbg);
    }

    // ── Difiere para el log estructurado (spdlog) desde el game tick ──
    DeferredAddOrder e{};
    e.character = charU;
    e.subject   = reinterpret_cast<uintptr_t>(subject);
    e.ai        = ai;
    e.aiSub     = aiSub;
    e.taskType  = taskType;
    e.arm5BD    = arm5BD;
    e.arm5BE    = arm5BE;
    e.carryArm  = carryArm;
    e.ret       = ret ? 1 : 0;
    e.callOk    = callOk ? 1 : 0;
    PushAddOrder(e);

    // Devuelve EXACTAMENTE lo que devolvió el original (si el trampoline falló,
    // false = "no manejada" → el caller sigue el pipeline normal, degradación segura).
    return ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  DIAG-COMBATSEED: Hook de diagnóstico en CombatClass::update(float) 0x60D650
//
//  SOLO LECTURA/LOG — llama al original con los MISMOS args (this=rcx, dt=xmm1) y NO
//  modifica ningún valor del juego. Prólogo limpio (sin mov rax,rsp) → MinHook normal, así
//  que el body puede leer memoria con Memory::Read (SEH) igual que Hook_AddOrderBackend.
//
//  Qué diagnostica: CombatClass::update es el AI tick de combate que consume el array de
//  percepciones (CombatClass+0x208, contador +0x200) para producir currentTarget
//  (CombatClass+0x290) y disparar Task_MeleeAttack. La hipótesis (2 agentes RE) es que en el
//  host recién reclamado ese "arranque" no ocurre en frío y sí tras golpear el muñeco. Este
//  hook captura, SOLO para el CombatClass del host, el estado en cada tick (throttled ~1.5s +
//  siempre en firstSight) para comparar en el log "frío" vs "tras entrenar" vs "atacando OK" y
//  localizar QUÉ campo cambia — el que luego habrá que sembrar al reclamar el char.
//
//  El CombatClass del host lo publica el hilo de lógica (PublishHostCombatDiag →
//  g_hostCombatClassForDiag); el hook solo compara su `this` contra esa caché (fast path para
//  los NPCs: una carga atómica + comparación + llamada al original).
// ═══════════════════════════════════════════════════════════════════════════
static void __fastcall Hook_CombatClassUpdate(void* combatClass, float dt) {
    uintptr_t cc = reinterpret_cast<uintptr_t>(combatClass);
    uintptr_t hostCC = g_hostCombatClassForDiag.load(std::memory_order_acquire);

    // Fast path: solo instrumentamos el CombatClass del HOST. Para el resto (NPCs) llamamos al
    // original y salimos — el tick corre para muchos personajes cada frame, coste mínimo aquí.
    if (hostCC == 0 || cc != hostCC) {
        SafeCall_Void_PtrF(reinterpret_cast<void*>(s_origCombatClassUpdate),
                           combatClass, dt, &s_combatSeedHealth);
        return;
    }

    // ── Es el tick del CombatClass del host ──
    // firstSight: el puntero del CombatClass del host cambió respecto al último visto → es una
    // instancia nueva (FIX-COMBATCLASS la creó, o el char se re-reclamó). Reinicia el contador
    // para que "seen" mida ticks DESDE ese instante (permite leer la secuencia frío→entreno→OK).
    uintptr_t prevCC = s_lastSeenHostCC.exchange(cc, std::memory_order_acq_rel);
    uint8_t firstSight = (prevCC != cc) ? 1 : 0;
    if (firstSight) s_hostCCSeenCount.store(0, std::memory_order_relaxed);
    uint32_t seen = s_hostCCSeenCount.fetch_add(1, std::memory_order_relaxed) + 1;

    // Throttle: SIEMPRE en firstSight (captura el instante del reclamo/creación); el resto, 1 vez
    // cada ~1.5s (el tick corre ~1/frame para el host y no queremos inundar el log).
    uint64_t now = GetTickCount64();
    uint64_t last = s_lastSeedLogMs.load(std::memory_order_relaxed);
    bool doLog = firstSight || (now - last >= 1500);

    if (doLog) {
        s_lastSeedLogMs.store(now, std::memory_order_relaxed);

        // Lecturas SEH-safe de los campos del CombatClass del host (0 = ilegible). Se leen ANTES
        // del original: capturan el estado persistente que dejó el tick anterior (currentTarget,
        // contador de percepciones), que es justo lo que cambia entre "frío" y "tras entrenar".
        uintptr_t f1F0 = 0, target290 = 0, ai28 = 0;
        uint32_t  cnt200 = 0;
        uint8_t   readOk = 1;
        if (!Memory::Read(cc + 0x1F0, f1F0))      readOk = 0; // campo previo al array de percepciones
        if (!Memory::Read(cc + 0x200, cnt200))    readOk = 0; // contador de percepciones de combate
        if (!Memory::Read(cc + 0x290, target290)) readOk = 0; // currentTarget (lo produce el AI tick)
        uintptr_t ai = g_hostAiForDiag.load(std::memory_order_acquire);
        if (ai) Memory::Read(ai + 0x28, ai28);                // AI+0x28: AttackState inline (target encolado)

        // Log inmediato barato (buffer de pila, sin heap) — visible en DebugView.
        char dbg[256];
        sprintf_s(dbg, "[DIAG-COMBATSEED] host CC=0x%llX %s seen=%u dt=%.3f | perc(+0x200)=%u "
                  "currentTarget(+0x290)=0x%llX +0x1F0=0x%llX AI+0x28=0x%llX%s\n",
                  (unsigned long long)cc, firstSight ? "[FIRST-SIGHT]" : "",
                  seen, (double)dt, cnt200,
                  (unsigned long long)target290, (unsigned long long)f1F0,
                  (unsigned long long)ai28, readOk ? "" : " [READ-FAIL]");
        OutputDebugStringA(dbg);

        // Difiere para el log estructurado (spdlog) desde el game tick (contexto seguro).
        DeferredCombatSeed e{};
        e.combatClass = cc;
        e.f1F0        = f1F0;
        e.cnt200      = cnt200;
        e.target290   = target290;
        e.ai28        = ai28;
        e.seenCount   = seen;
        e.firstSight  = firstSight;
        e.readOk      = readOk;
        PushCombatSeed(e);
    }

    // Llama al original SIN modificar — el AI tick debe ejecutarse exactamente igual.
    SafeCall_Void_PtrF(reinterpret_cast<void*>(s_origCombatClassUpdate),
                       combatClass, dt, &s_combatSeedHealth);
}

// [DIAG-COMBATSEED] Publica el CombatClass y el AI del host para que el hook los use.
// Corre en el HILO DE LÓGICA (ProcessDeferredEvents), throttle 250ms. Resuelve el char primario
// del host y su cadena CharBody(*(char+0x648)) → CombatClass(*(CharBody+0x8)), y AI(*(char+0x650)),
// todo con Memory::Read SEH-safe. Si algún paso falla, deja la caché como está (el hook simplemente
// no marca ticks hasta que se resuelva). NO escribe memoria del juego.
static void PublishHostCombatDiag(Core& core) {
    if (!core.IsHost()) return;

    static uint64_t s_lastPublishMs = 0;
    uint64_t now = GetTickCount64();
    if (now - s_lastPublishMs < 250) return;
    s_lastPublishMs = now;

    uintptr_t hostChar = game::GetPlayerPrimaryCharacterDirect();
    if (hostChar <= 0x10000) return;   // char aún no resoluble (menú/carga) → reintentar

    uintptr_t charBody = 0, combatClass = 0, ai = 0;
    if (Memory::Read(hostChar + 0x648, charBody) && charBody > 0x10000) {
        if (Memory::Read(charBody + 0x8, combatClass)) {
            g_hostCombatClassForDiag.store(combatClass, std::memory_order_release);
        }
    }
    if (Memory::Read(hostChar + 0x650, ai)) {
        g_hostAiForDiag.store(ai, std::memory_order_release);
    }
}

// [DIAG-COMBATSEED] Vaciado del ring (log estructurado spdlog, contexto seguro de game tick).
// Una línea por snapshot con TODO: puntero del CombatClass, contador de percepciones, currentTarget,
// AI+0x28 y las marcas firstSight/seen para reconstruir la secuencia frío→entreno→ataque OK.
static void DrainCombatSeedRing() {
    DeferredCombatSeed cs;
    int n = 0;
    while (PopCombatSeed(cs) && n < 32) { // cap por tick para no atascar el game tick
        n++;
        spdlog::info("[DIAG-COMBATSEED] host CC=0x{:X}{} seen={} | perc(+0x200)={} "
                     "currentTarget(+0x290)=0x{:X} +0x1F0=0x{:X} AI+0x28=0x{:X}{}",
                     cs.combatClass, cs.firstSight ? " [FIRST-SIGHT]" : "", cs.seenCount,
                     cs.cnt200, cs.target290, cs.f1F0, cs.ai28,
                     cs.readOk ? "" : "  [READ-FAIL]");
    }
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

// ── DIAG-ADDORDER-BACKEND: vaciado del ring (log estructurado, contexto seguro) ──
// Compartido por la rama offline y la conectada: el DIAG del validador aplica igual
// en single-player del host (el bug de órdenes tragadas no depende de la conexión).
// Una sola línea por evento con TODO: taskType, punteros, bytes médicos y veredicto.
static void DrainAddOrderRing() {
    DeferredAddOrder ao;
    int aoN = 0;
    while (PopAddOrder(ao) && aoN < 32) { // cap por tick para no atascar el game tick
        aoN++;
        spdlog::info("[DIAG-ADDORDER-BACKEND] taskType=0x{:X} char=0x{:X} subject=0x{:X} "
                     "arm5BD={} arm5BE={} ai=0x{:X} aiSub=0x{:X} carryArm={} ret={}{}",
                     (unsigned)ao.taskType, ao.character, ao.subject,
                     (int)ao.arm5BD, (int)ao.arm5BE, ao.ai, ao.aiSub, (int)ao.carryArm,
                     ao.ret ? "ABORTADA (orden tragada)" : "OK (orden continua)",
                     ao.callOk ? "" : " [TRAMPOLINE FAIL]");
    }
}

void ProcessDeferredEvents() {
    auto& core = Core::Get();

    // [DIAG-COMBATSEED] Publica el CombatClass/AI del host para el hook de CombatClass::update.
    // Va lo PRIMERO (antes del check de conexión) porque aplica igual en single-player del host.
    PublishHostCombatDiag(core);

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
        // DIAG-ADDORDER-BACKEND también offline: el validador traga órdenes del host
        // igual sin conexión — es justo el escenario del test de esta noche.
        DrainAddOrderRing();
        // DIAG-COMBATSEED también offline: el host puede atacar sin conexión y queremos capturar
        // el estado del CombatClass en frío vs tras entrenar igual (el AI tick corre offline).
        DrainCombatSeedRing();
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

    // ── DIAG-ADDORDER-BACKEND: vaciado del ring (rama conectada) ──
    DrainAddOrderRing();

    // ── DIAG-COMBATSEED: vaciado del ring (rama conectada) ──
    DrainCombatSeedRing();

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

// ═══════════════════════════════════════════════════════════════════════════
//  FIX-UAF-BUYITEM (0x74A630) — MOVIDO A inventory_hooks.cpp (2026-07-14)
//
//  El guard del use-after-free del retorno de BuyItem (antes Hook_BuyItemUafGuard,
//  con s_origBuyItemUafGuard / InitGameCodeRangeForBuyItemGuard / IsHeapPtrForBuyItemGuard)
//  se ELIMINÓ de aquí y se FUSIONÓ con el hook de sincronización de red de BuyItem que ya
//  vivía en inventory_hooks.cpp (Hook_BuyItem). Motivo: ambos hooks apuntaban a la MISMA RVA
//  0x74A630 y MinHook deduplica por dirección — el segundo MH_CreateHook se rechazaba en
//  silencio (MH_ERROR_ALREADY_CREATED), dejando uno de los dos SIN instalar según el orden de
//  Install(). Ahora hay UN solo detour sobre 0x74A630 (en inventory_hooks.cpp) que hace tanto
//  el guard UAF (validación de la vtable del puntero devuelto) como la sync de red. Además se
//  corrigió allí la firma del retorno (era `char`, truncaba el puntero de 64 bits a 1 byte —
//  causa directa del crash game+0x9A18DA; el real es `void*`). Temáticamente BuyItem es de
//  comercio/inventario, no de combate, así que inventory_hooks.cpp es su sitio correcto.
// ═══════════════════════════════════════════════════════════════════════════

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

    // ═══ DIAG-ADDORDER-BACKEND en Character::addOrder backend 0x5D1940 ═══
    // El VALIDADOR que traga órdenes (RE 2026-07-11/12): si el chequeo de brazos falla,
    // devuelve true SIN encolar → la orden de atacar/comer/cargar del host desaparece en
    // silencio. Prólogo limpio (40 55 56 57, sin mov rax,rsp) → MinHook normal.
    // Se instala ACTIVO desde el arranque (a diferencia de PushOrder): el test de esta
    // noche necesita capturar las órdenes desde el primer momento, sin flag condicional.
    // SOLO registra (reenvía args idénticos y devuelve el retorno del original tal cual).
    if (funcs.AddOrderBackend) {
        hookMgr.InstallAt("AddOrderBackend",
                          reinterpret_cast<uintptr_t>(funcs.AddOrderBackend),
                          &Hook_AddOrderBackend, &s_origAddOrderBackend);
        spdlog::info("combat_hooks: [DIAG-ADDORDER-BACKEND] hook instalado en el validador de "
                     "ordenes 0x{:X} (ACTIVO desde el arranque; true=orden tragada)",
                     reinterpret_cast<uintptr_t>(funcs.AddOrderBackend));
    } else {
        spdlog::warn("combat_hooks: [DIAG-ADDORDER-BACKEND] funcs.AddOrderBackend no resuelto — "
                     "DIAG no disponible");
    }

    // ═══ DIAG-COMBATSEED en CombatClass::update(float) 0x60D650 ═══
    // Hook de SOLO diagnóstico del AI tick de combate. Prólogo limpio (40 53 48 83 EC 20 48 8B D9,
    // sin mov rax,rsp) → MinHook normal. Se instala DESHABILITADO y se activa al cargar el juego
    // (mismas rutas que PushOrder: Core::SetConnected / OnGameLoaded → Enable("CombatClassUpdate")),
    // porque CombatClass::update dispara para TODOS los personajes cada frame y no queremos que
    // corra durante las 130+ creaciones de la carga. SOLO lee/loguea (no muta el AI tick).
    if (funcs.CombatClassUpdate) {
        hookMgr.InstallAt("CombatClassUpdate",
                          reinterpret_cast<uintptr_t>(funcs.CombatClassUpdate),
                          &Hook_CombatClassUpdate, &s_origCombatClassUpdate);
        HookManager::Get().Disable("CombatClassUpdate"); // arranca OFF; se activa al cargar el juego
        spdlog::info("combat_hooks: [DIAG-COMBATSEED] hook instalado en CombatClass::update 0x{:X} "
                     "(arranca deshabilitado)", reinterpret_cast<uintptr_t>(funcs.CombatClassUpdate));
    } else {
        spdlog::warn("combat_hooks: [DIAG-COMBATSEED] funcs.CombatClassUpdate no resuelto — "
                     "DIAG no disponible");
    }

    // ═══ FIX-UAF-BUYITEM: MOVIDO a inventory_hooks.cpp (2026-07-14) ═══
    // El guard del retorno de BuyItem (0x74A630) ya NO se instala aquí. Se fusionó con el hook de
    // sync de red de BuyItem en inventory_hooks.cpp para evitar la colisión de dos hooks sobre la
    // misma RVA (MinHook deduplica por dirección → el segundo se rechazaba en silencio). El único
    // detour sobre 0x74A630 lo instala ahora inventory_hooks::Install().

    spdlog::info("combat_hooks: Installed (damage=SKIPPED, death={}, ko={}, issueOrder_DIAG=DISABLED, "
                 "pushOrder_DIAG={}, addOrderBackend_DIAG={}, combatSeed_DIAG={})",
                 funcs.CharacterDeath != nullptr, funcs.CharacterKO != nullptr,
                 funcs.PushOrder != nullptr, funcs.AddOrderBackend != nullptr,
                 funcs.CombatClassUpdate != nullptr);
    return true;
}

void Uninstall() {
    HookManager::Get().Remove("CharacterDeath");
    HookManager::Get().Remove("CharacterKO");
    HookManager::Get().Remove("StartAttack"); // DIAG-COMBAT
    HookManager::Get().Remove("PushOrder");        // DIAG-PUSHORDER
    HookManager::Get().Remove("AddOrderBackend");  // DIAG-ADDORDER-BACKEND
    HookManager::Get().Remove("CombatClassUpdate"); // DIAG-COMBATSEED
    // FIX-UAF-BUYITEM: el hook de BuyItem (guard UAF + sync de red) lo desinstala ahora
    // inventory_hooks::Uninstall() ("BuyItem"), ya no hay "BuyItemUafGuard" que quitar aquí.
    spdlog::info("combat_hooks: Uninstalled");
}

void SetServerSourcedDeath(bool active) {
    s_serverSourcedDeath.store(active, std::memory_order_release);
}

void SetServerSourcedKO(bool active) {
    s_serverSourcedKO.store(active, std::memory_order_release);
}

} // namespace kmp::combat_hooks
