// ES: core.cpp — Implementación del singleton kmp::Core (corazón de KenshiMP.Core.dll).
//     Contiene: diagnóstico de crashes (VEH, migas de pan, atexit/SIGABRT), el arranque
//     Initialize() (logging, escáner de patrones, hooks MinHook, red ENet, UI, hilo de red),
//     Shutdown(), la máquina de fases del cliente, la detección de carga de partida
//     (PollForGameLoad / OnLoadingGapDetected), OnGameLoaded(), el bucle por frame OnGameTick()
//     y los fixes one-shot del personaje del host (facción, relaciones, semillas de tiempo, control,
//     CombatClass, platoon, brazo, clon de escuadrón...), además del pipeline de posiciones,
//     la cola de spawns y los comandos de escape (/tp, /forcespawn).
//     Referencias: docs/architecture/01-core-lifecycle.md, 06-game-offsets.md y audit-*.md.
// EN: core.cpp — Implementation of the kmp::Core singleton (heart of KenshiMP.Core.dll).
//     Contains: crash diagnostics (VEH, breadcrumbs, atexit/SIGABRT), Initialize() startup
//     (logging, pattern scanner, MinHook hooks, ENet network, UI, network thread), Shutdown(),
//     the client phase machine, game-load detection (PollForGameLoad / OnLoadingGapDetected),
//     OnGameLoaded(), the per-frame OnGameTick() loop and the one-shot fixes for the host
//     character (faction, relations, time seeds, control, CombatClass, platoon, arm, squad clone...),
//     plus the position pipeline, the spawn queue and the escape-hatch commands (/tp, /forcespawn).
//     References: docs/architecture/01-core-lifecycle.md, 06-game-offsets.md and audit-*.md.
#include "core.h"
#include "sync/pending_snapshot_queue.h"
#include "sys/command_registry.h"
#include "hooks/render_hooks.h"
#include "hooks/input_hooks.h"
#include "hooks/entity_hooks.h"
#include "hooks/movement_hooks.h"
#include "hooks/combat_hooks.h"
#include "hooks/world_hooks.h"
#include "hooks/save_hooks.h"
#include "hooks/time_hooks.h"
#include "hooks/game_tick_hooks.h"
#include "hooks/inventory_hooks.h"
#include "hooks/squad_hooks.h"
#include "hooks/faction_hooks.h"
#include "hooks/building_hooks.h"
#include "hooks/ai_hooks.h"
#include "hooks/resource_hooks.h"
#include "hooks/squad_spawn_hooks.h"
#include "hooks/char_tracker_hooks.h"
#include "game/game_types.h"
#include "game/game_offset_prober.h"
#include "game/asset_facilitator.h"
#include "game/shared_save_sync.h"
#include "game/game_inventory.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/constants.h"
#include "kmp/memory.h"
#include "kmp/function_analyzer.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>   // [FIX-HOSTREL] dedup de nodos del boost::unordered_map (cadena 'next' compartida)
#include <csignal>
#include <cstring>   // strcmp / memcpy (usados por SEH_ReadMsvcStr / SEH_IsNamelessFaction)
#include <Windows.h>

namespace kmp {

// ES: Declaración adelantada (registra los handlers de paquetes; definida en packet_handler.cpp).
// Forward declaration (in kmp namespace)
void InitPacketHandler();

// ── ValidateGameWorldGlobal (Steam 1.0.68: instancia embebida vs puntero) ──
// CRITICO: en Steam 1.0.68 GameWorld NO es un puntero global (GameWorld* ou) sino la
// INSTANCIA estatica embebida en .data. Por tanto la direccion resuelta (base+0x2134110)
// ES directamente el objeto GameWorld; su primer qword es la vtable en .text, NO un heap-ptr.
// El validador generico (validateGlobal) exige que *addr sea heap FUERA del modulo y por eso
// RECHAZABA la instancia embebida (su primer qword cae dentro del modulo). Aqui aceptamos
// AMBOS layouts, robusto a version/plataforma:
//   (a) puntero clasico  : *addr es heap-ptr a un objeto con vtable en .text -> valido
//   (b) instancia directa: *addr ES la vtable en .text (la instancia esta embebida) -> valido
// Devuelve true si 'addr' apunta/contiene un objeto GameWorld plausible. NO sigue la cadena
// player/faction (eso lo hace el Scanner); aqui basta el test barato vtable/heap.
// EN: ── ValidateGameWorldGlobal (Steam 1.0.68: embedded instance vs pointer) ──
//     CRITICAL: in Steam 1.0.68 GameWorld is NOT a global pointer (GameWorld* ou) but the static
//     INSTANCE embedded in .data. So the resolved address (base+0x2134110) IS the GameWorld object
//     itself; its first qword is the vtable in .text, NOT a heap pointer. The generic validator
//     (validateGlobal) requires *addr to be heap OUTSIDE the module and therefore REJECTED the
//     embedded instance. Here we accept BOTH layouts, robust to version/platform:
//       (a) classic pointer : *addr is a heap pointer to an object with a vtable in .text -> valid
//       (b) direct instance : *addr IS the vtable in .text (the instance is embedded) -> valid
//     Returns true if addr points to / contains a plausible GameWorld object. It does NOT follow the
//     player/faction chain (the Scanner does that); the cheap vtable/heap test is enough here.
static bool ValidateGameWorldGlobal(uintptr_t addr, uintptr_t moduleBase, size_t moduleSize) {
    if (addr == 0) return false;
    uintptr_t first = 0;
    if (!Memory::Read(addr, first)) return false;
    if (first == 0) return false; // Game not loaded yet
    uintptr_t textStart = moduleBase + 0x1000;
    uintptr_t textEnd   = moduleBase + moduleSize;
    // Caso (b): instancia directa — el primer qword YA es la vtable en .text.
    if (first >= textStart && first < textEnd) return true;
    // Caso (a): puntero clasico — *addr es heap-ptr fuera del modulo; su objeto debe tener
    // vtable en .text.
    if (first < 0x10000 || first >= 0x00007FFFFFFFFFFF) return false;
    if (first >= moduleBase && first < moduleBase + moduleSize) return false; // dentro modulo: no es heap-ptr
    uintptr_t vtable = 0;
    if (!Memory::Read(first, vtable)) return false;
    return (vtable >= textStart && vtable < textEnd);
}

// ES: ── Diagnóstico de crashes ──
//     Guarda el último paso completado de OnGameTick para que el manejador de crashes lo reporte.
//     Son volatile porque los lee el VEH desde cualquier hilo en mitad de una excepción.
// EN: They are volatile because the VEH reads them from any thread in the middle of an exception.
// ── Crash diagnostics ──
// Tracks last completed step in OnGameTick so the crash handler can report it.
static volatile int g_lastTickStep = -1;
static volatile int g_tickNumber = 0;
static volatile const char* g_lastStepName = "init";

// ES: Manejador vectorizado de excepciones (VEH) — se dispara ANTES que los manejadores SEH por
//     frame de Kenshi. Kenshi sobrescribía SetUnhandledExceptionFilter y nunca veíamos el crash.
//     g_gameModuleBase/End = rango de kenshi_x64.exe en memoria (para filtrar por RIP).
// EN: g_gameModuleBase/End = range of kenshi_x64.exe in memory (used to filter by RIP).
// Vectored exception handler — fires BEFORE Kenshi's frame-based SEH handlers.
// SetUnhandledExceptionFilter was being overridden by Kenshi, so we never saw crash info.
static PVOID g_vehHandle = nullptr;
static volatile bool g_vehFired = false;
static uintptr_t g_gameModuleBase = 0;
static uintptr_t g_gameModuleEnd  = 0;

// ES: Contador exportado — entity_hooks lo actualiza para que el VEH sepa qué CharacterCreate petó.
// Exported counter — entity_hooks updates this so VEH can report which create crashed
volatile int g_lastCharacterCreateNum = 0;

// ES: ── Rastro de migas de pan para diagnosticar crashes ──
//     Escribe a un fichero ANTES de cada operación peligrosa. Si el proceso muere por __fastfail,
//     TerminateProcess o una excepción imposible de capturar, el fichero muestra el último paso.
//     Usa E/S de C directa con fflush para escritura inmediata. Solo escribe los primeros 50 ticks
//     y luego 1 de cada 10. OJO: ruta absoluta fija a la instalación de Steam en Program Files.
// EN: Only writes the first 50 ticks and then 1 in 10. NOTE: hard-coded absolute path to the
//     Steam install under Program Files.
// ── Breadcrumb trail for crash diagnosis ──
// Writes to a file BEFORE each dangerous operation. If the process dies from
// __fastfail, TerminateProcess, or any uncatchable exception, the file shows
// the last known step. Uses direct C I/O with fflush for immediate write.
static void WriteBreadcrumb(const char* step, int tickNum = 0, int extra = 0) {
    // Write every Nth tick to avoid excessive I/O, PLUS always write the first 50
    static int s_writeCount = 0;
    if (tickNum > 50 && tickNum % 10 != 0) return; // Skip most ticks after warmup
    s_writeCount++;

    FILE* f = nullptr;
    fopen_s(&f, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Kenshi\\KenshiOnline_BREADCRUMB.txt", "w");
    if (f) {
        fprintf(f, "tick=%d step=%s extra=%d charCreate=#%d writes=%d\n",
                tickNum, step, extra, g_lastCharacterCreateNum, s_writeCount);
        fflush(f);
        fclose(f);
    }
}

// ES: Volcado de pila seguro con SEH (no se permiten objetos C++ con destructor dentro de __try):
//     imprime 16 qwords desde RSP en outBuf; devuelve la longitud escrita.
// EN: Prints 16 qwords from RSP into outBuf; returns the written length.
// SEH-safe stack dump helper (no C++ objects with destructors allowed)
static int SEH_DumpStack(char* outBuf, int outBufSize, uint64_t rsp) {
    int pos = sprintf_s(outBuf, outBufSize, "  Stack at RSP:\n");
    __try {
        auto* sp = reinterpret_cast<const uint64_t*>(rsp);
        for (int i = 0; i < 16 && pos < outBufSize - 64; i++) {
            pos += sprintf_s(outBuf + pos, outBufSize - pos,
                "    [RSP+0x%02X] = 0x%016llX\n", i * 8, sp[i]);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        pos += sprintf_s(outBuf + pos, outBufSize - pos,
            "    (stack read failed)\n");
    }
    return pos;
}

// ES: Manejador VEH de crashes. Filtra por tipo de excepción y por dónde está el RIP (juego, nuestra
//     DLL, stubs de hooks, NULL, Ogre/MyGUI; excluye PhysX), ignora las AV benignas de las sondas SEH
//     de la DLL, deduplica por RIP (máx. 3) y limita a 20 entradas por sesión. Registra registros y
//     pila con E/S ligera (sin spdlog para no bloquear mutex) en KenshiOnline_CRASH.log.
//     Siempre devuelve EXCEPTION_CONTINUE_SEARCH: solo registra, no maneja la excepción.
// EN: VEH crash handler. Filters by exception type and by where RIP lands (game, our DLL, hook stubs,
//     NULL, Ogre/MyGUI; excludes PhysX), ignores benign AVs from the DLL SEH probes, dedups by RIP
//     (max 3) and caps at 20 entries per session. Logs registers and stack with lightweight I/O
//     (no spdlog, to avoid mutex deadlocks) to KenshiOnline_CRASH.log.
//     Always returns EXCEPTION_CONTINUE_SEARCH: it only logs, it never handles the exception.
static LONG CALLBACK VectoredCrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // ES: Solo interesan excepciones fatales + corrupción de heap / excepciones C++ (códigos abajo).
    // Handle fatal exception types + heap/C++ exceptions for crash diagnosis.
    // 0xC0000374 = STATUS_HEAP_CORRUPTION, 0xC0000602 = STATUS_FAIL_FAST_EXCEPTION,
    // 0xE06D7363 = MSVC C++ exception (throw), 0xC0000409 = STATUS_STACK_BUFFER_OVERRUN
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != 0xC0000374 &&  // STATUS_HEAP_CORRUPTION
        code != 0xC0000602 &&  // STATUS_FAIL_FAST_EXCEPTION
        code != 0xC0000409 &&  // STATUS_STACK_BUFFER_OVERRUN (/GS check)
        code != 0xE06D7363) {  // MSVC C++ exception
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // ES: ── Filtro por la ubicación del RIP ──
    //     Las DLL de sistema generan AV durante el funcionamiento normal (sondeo de memoria, guard pages)
    //     y el SEH de Kenshi las captura. Solo registramos crashes en: módulo del juego, nuestra DLL,
    //     stubs de hooks reservados dinámicamente o NULL.
    // ── Filter by RIP location ──
    // System DLLs (ntdll, ucrtbase, etc.) trigger AVs during normal operation
    // (e.g., memory probing, guard page checks). Kenshi's SEH catches these.
    // Log crashes in: game module, our DLL, dynamically allocated hook stubs, or NULL.
    uintptr_t rip = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    bool inGame = (rip >= g_gameModuleBase && rip < g_gameModuleEnd);
    bool isNull = (rip < 0x10000);  // NULL or near-NULL pointer call

    // ES: Aceptar también crashes dentro de nuestra DLL (KenshiMP.Core.dll); su rango se calcula una vez
    //     leyendo SizeOfImage de la cabecera PE.
    // Also accept crashes in our DLL module (KenshiMP.Core.dll)
    static uintptr_t s_dllBase = 0, s_dllEnd = 0;
    if (s_dllBase == 0) {
        HMODULE ourDll = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(&VectoredCrashHandler), &ourDll);
        if (ourDll) {
            s_dllBase = reinterpret_cast<uintptr_t>(ourDll);
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(ourDll);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const uint8_t*>(ourDll) + dos->e_lfanew);
            s_dllEnd = s_dllBase + nt->OptionalHeader.SizeOfImage;
        }
    }
    bool inOurDll = (s_dllBase && rip >= s_dllBase && rip < s_dllEnd);

    // ES: Aceptar crashes en páginas VirtualAlloc de direcciones bajas (stubs de hooks en
    //     0x01000000-0x7FFFFFFF). Las DLL del sistema están en 0x7FFE..., fuera de este rango.
    // Accept crashes in low-address VirtualAlloc'd pages (hook stubs at 0x01000000-0x7FFFFFFF)
    // Hook pages are allocated via VirtualAlloc with PAGE_EXECUTE_READWRITE at low addresses.
    // System DLLs are at 0x7FFE... which is above this range — no false positives.
    bool inHookStub = (rip >= 0x10000 && rip < 0x80000000);

    // ES: Detectar también crashes en las DLL de Ogre/MyGUI (nuestro código interactúa con ellas).
    //     PhysX se resuelve para EXCLUIRLO: PhysXCore64.dll carga en ~0x70800000, dentro del rango de
    //     stubs, y sus crashes no los causa nuestro código.
    // Also detect crashes in Ogre/MyGUI DLLs — our code interacts with these,
    // and crashes there are invisible without explicit tracking.
    // PhysX is resolved to EXCLUDE it from inHookStub — PhysXCore64.dll loads at
    // ~0x70800000 which falls inside the 0x10000..0x80000000 hook stub range.
    // PhysX crashes aren't caused by our code and should not be logged.
    static uintptr_t s_ogreBase = 0, s_ogreEnd = 0;
    static uintptr_t s_myguiBase = 0, s_myguiEnd = 0;
    static uintptr_t s_physxBase = 0, s_physxEnd = 0;
    static uintptr_t s_physx3Base = 0, s_physx3End = 0;
    static bool s_gameDllsResolved = false;
    if (!s_gameDllsResolved) {
        s_gameDllsResolved = true;
        auto resolveModule = [](const char* name, uintptr_t& base, uintptr_t& end) {
            HMODULE h = GetModuleHandleA(name);
            if (h) {
                base = reinterpret_cast<uintptr_t>(h);
                auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
                auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                    reinterpret_cast<const uint8_t*>(h) + dos->e_lfanew);
                end = base + nt->OptionalHeader.SizeOfImage;
            }
        };
        resolveModule("OgreMain_x64.dll", s_ogreBase, s_ogreEnd);
        resolveModule("MyGUIEngine_x64.dll", s_myguiBase, s_myguiEnd);
        resolveModule("PhysXCore64.dll", s_physxBase, s_physxEnd);
        resolveModule("PhysX3_x64.dll", s_physx3Base, s_physx3End);
    }
    bool inOgre = (s_ogreBase && rip >= s_ogreBase && rip < s_ogreEnd);
    bool inMyGUI = (s_myguiBase && rip >= s_myguiBase && rip < s_myguiEnd);
    bool inPhysX = (s_physxBase && rip >= s_physxBase && rip < s_physxEnd) ||
                   (s_physx3Base && rip >= s_physx3Base && rip < s_physx3End);

    // ES: Excluir PhysX de la detección de stubs — sus crashes no son culpa nuestra.
    // Exclude PhysX from hook stub detection — PhysX crashes are not our fault
    if (inPhysX) inHookStub = false;

    if (!inGame && !isNull && !inOurDll && !inHookStub && !inOgre && !inMyGUI) {
        return EXCEPTION_CONTINUE_SEARCH;  // System DLL exception — let SEH handle it
    }

    // ES: Saltar las AV internas de la DLL que son sondas benignas protegidas con SEH (Memory::Read
    //     genera ~53 AV por sesión, el 84% de las entradas del VEH). Pero NO saltar todas las AV de la DLL:
    //     los bugs reales de hooks deben registrarse. Estrategia: saltar por rangos de RVA conocidos de la
    //     DLL y durante la carga (tick=0), cuando ocurren todas las sondas del escáner.
    //     OJO: estos rangos (dll+0x1A00..0x1D00, +0x2BD00..0x2D000, +0x21000..0x22200) dependen del
    //     layout de cada build de la DLL; pueden quedar desfasados si cambia el código.
    // EN: NOTE: these ranges (dll+0x1A00..0x1D00, +0x2BD00..0x2D000, +0x21000..0x22200) depend on the
    //     layout of each DLL build; they can become stale when the code changes.
    // Skip DLL-internal access violations that are benign SEH-protected probes.
    // Memory::Read probes trigger ~53 AVs per session (84% of all VEH entries).
    // These are caught by __except handlers and are completely harmless.
    // BUT we must NOT skip all DLL AVs — real hook bugs need to be logged.
    // Strategy: skip DLL AVs at known Memory::Read RVA ranges (dll+0x1B00..0x1C00)
    // and during loading (tick=0) when all the scanner probes happen.
    if (inOurDll && code == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t dllOffset = rip - s_dllBase;
        // Memory::Read<T> templates are at dll+0x1B00..0x1C00 (all builds)
        if (dllOffset >= 0x1A00 && dllOffset <= 0x1D00) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // SEH-protected functions: SEH_ReadPointer, SEH_ReadKenshiStringRaw,
        // SEH_CallFactory, ScanGameDataHeap, and other SEH helpers in spawn_manager.
        // These intentionally trigger AVs while probing memory — caught by __except.
        // Range 0x2BD00..0x2D000 covers all spawn_manager SEH functions.
        if (dllOffset >= 0x2BD00 && dllOffset <= 0x2D000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // SEH helpers in entity_hooks (SEH_ReadCharacterData, SEH_ReadFactionPtr, etc.)
        // Range 0x21000..0x22200 covers all entity_hooks SEH functions.
        if (dllOffset >= 0x21000 && dllOffset <= 0x22200) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // SEH helpers in sync_orchestrator (SEH_WritePositionRotation, SEH_ReadPosition, etc.)
        // These probe game objects that may be freed. Range 0x0..0x2000 from sync_orch start.
        // Broad skip: any DLL AV in an __except-protected function is benign.
        // During loading (before first game tick), most DLL AVs are scanner probes
        if (g_tickNumber == 0) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    // ES: Deduplicar — no inundar el log con crashes idénticos repetidos (mismo RIP).
    // Deduplicate — don't flood the log with repeated identical crashes (same RIP)
    static volatile uintptr_t s_lastCrashRip = 0;
    static volatile int s_repeatCount = 0;
    if (rip == s_lastCrashRip) {
        if (++s_repeatCount > 2) return EXCEPTION_CONTINUE_SEARCH; // Log max 3 per RIP
    } else {
        s_lastCrashRip = rip;
        s_repeatCount = 0;
    }

    // ES: Límite de registro: hasta 20 entradas por sesión. (El antiguo one-shot g_vehFired se consumía
    //     con el ruido de SEH_MemcpySafe al arrancar y el crash REAL post-carga quedaba sin registrar.)
    // Rate-limit crash logging — allow up to 20 entries per session.
    // (Old one-shot g_vehFired was consumed by SEH_MemcpySafe noise at startup,
    //  causing the REAL post-loading crash to go completely unlogged!)
    static volatile int s_vehEntryCount = 0;
    if (s_vehEntryCount >= 20) return EXCEPTION_CONTINUE_SEARCH;
    s_vehEntryCount++;
    g_vehFired = (s_vehEntryCount >= 20);

    // ES: ── SOLO REGISTRO LIGERO ── ¡Nada de spdlog aquí! spdlog toma mutex internos; si el crash ocurrió
    //     dentro de una llamada a spdlog, volver a tomarlo provoca deadlock. Solo OutputDebugStringA +
    //     escritura directa a fichero (sin objetos C++).
    // ── LIGHTWEIGHT LOGGING ONLY ──
    // NO spdlog calls here! spdlog acquires mutexes internally.
    // If the crash happened mid-spdlog-call, re-acquiring deadlocks.
    // Use only OutputDebugStringA + direct file write (no C++ objects).

    auto* ctx = ep->ContextRecord;
    uintptr_t ripOffset = inGame ? (rip - g_gameModuleBase) : 0;

    char buf[4096];
    int pos = sprintf_s(buf,
        "KMP VEH CRASH: ExceptionCode=0x%08lX at RIP=0x%016llX (game+0x%llX)\n"
        "  RAX=0x%016llX  RBX=0x%016llX  RCX=0x%016llX  RDX=0x%016llX\n"
        "  RSP=0x%016llX  RBP=0x%016llX  RSI=0x%016llX  RDI=0x%016llX\n"
        "  R8 =0x%016llX  R9 =0x%016llX  R10=0x%016llX  R11=0x%016llX\n"
        "  R12=0x%016llX  R13=0x%016llX  R14=0x%016llX  R15=0x%016llX\n"
        "  Last CharacterCreate: #%d, OnGameTick step: %d (%s), tick #%d\n"
        "  Filter: inGame=%d inNull=%d inDll=%d inStub=%d inOgre=%d inMyGUI=%d dllBase=0x%llX dllEnd=0x%llX\n",
        code, (unsigned long long)rip, (unsigned long long)ripOffset,
        ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx,
        ctx->Rsp, ctx->Rbp, ctx->Rsi, ctx->Rdi,
        ctx->R8, ctx->R9, ctx->R10, ctx->R11,
        ctx->R12, ctx->R13, ctx->R14, ctx->R15,
        g_lastCharacterCreateNum,
        g_lastTickStep, g_lastStepName ? g_lastStepName : "?", g_tickNumber,
        (int)inGame, (int)isNull, (int)inOurDll, (int)inHookStub,
        (int)inOgre, (int)inMyGUI,
        (unsigned long long)s_dllBase, (unsigned long long)s_dllEnd);

    // ES: Detalle de la violación de acceso: tipo (lectura/escritura/ejecución DEP) y dirección.
    // Log access violation details with correct type classification
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR avType = ep->ExceptionRecord->ExceptionInformation[0];
        const char* op = (avType == 0) ? "READ" : (avType == 1) ? "WRITE" : "DEP/EXECUTE";
        pos += sprintf_s(buf + pos, sizeof(buf) - pos,
            "  AV: %s at 0x%016llX\n", op,
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    }

    // ES: Volcado de pila: 16 qwords desde RSP (en función aparte segura con SEH).
    // Stack dump: 16 qwords from RSP (in separate SEH-safe function)
    if (ctx->Rsp > 0x10000 && ctx->Rsp < 0x00007FFFFFFFFFFF) {
        char stackBuf[1024];
        SEH_DumpStack(stackBuf, sizeof(stackBuf), ctx->Rsp);
        strcat_s(buf, stackBuf);
    }

    OutputDebugStringA(buf);

    // ES: Escribir al fichero de crashes dedicado (E/S de C directa, sin objetos C++). Ruta relativa
    //     al directorio de trabajo del juego.
    // EN: Relative path, resolved against the game working directory.
    // Write to dedicated crash file (direct C I/O, no C++ objects)
    FILE* f = nullptr;
    fopen_s(&f, "KenshiOnline_CRASH.log", "a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }

    g_vehFired = false; // Allow re-fire for subsequent crashes
    return EXCEPTION_CONTINUE_SEARCH; // Let Windows/Kenshi handle it
}

// ES: Singleton Meyers: la instancia única se crea en la primera llamada (thread-safe en C++11).
// EN: Meyers singleton: the single instance is created on first call (thread-safe since C++11).
Core& Core::Get() {
    static Core instance;
    return instance;
}

// ES: Arranque completo del mod (lo llama dllStartPlugin). Orden: logging → rango del módulo del
//     juego → VEH → cabecera de sesión y migas de pan → atexit/SIGABRT/filtro de excepciones →
//     detección Steam/GOG → config → offsets → escáner → MinHook + hooks → red → handlers de paquetes
//     → callback de spawn → UI → orquestadores → hilo de red. Devuelve false si falla algo crítico.
// EN: Full mod startup (called by dllStartPlugin). Order: logging → game module range → VEH →
//     session header and breadcrumbs → atexit/SIGABRT/exception filter → Steam/GOG detection →
//     config → offsets → scanner → MinHook + hooks → network → packet handlers → spawn callback →
//     UI → orchestrators → network thread. Returns false if something critical fails.
bool Core::Initialize() {
    // ES: Configurar el logging (fichero por PID para que varias instancias no se pisen).
    // Set up logging
    try {
        // PID-based log filename so multiple instances don't clobber each other
        DWORD pid = GetCurrentProcessId();
        std::string logFile = "KenshiOnline_" + std::to_string(pid) + ".log";
        auto logger = spdlog::basic_logger_mt("kenshi_online", logFile, true);
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::debug);
        spdlog::flush_on(spdlog::level::debug);
    } catch (...) {
        // Fallback: no file logging
    }

    // ES: Capturar el rango del módulo del juego (kenshi_x64.exe) para el filtrado del VEH.
    // Capture game module range for VEH filtering
    HMODULE gameModule = GetModuleHandleA(nullptr);  // kenshi_x64.exe
    if (gameModule) {
        g_gameModuleBase = reinterpret_cast<uintptr_t>(gameModule);
        // Read PE header to get SizeOfImage
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(gameModule);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const uint8_t*>(gameModule) + dos->e_lfanew);
        g_gameModuleEnd = g_gameModuleBase + nt->OptionalHeader.SizeOfImage;
    }

    // ES: Registrar el VEH — se dispara ANTES que los manejadores SEH de Kenshi (que sobrescribe
    //     SetUnhandledExceptionFilter). El filtro solo registra crashes relevantes (ver arriba).
    // Register vectored exception handler — fires BEFORE Kenshi's SEH handlers.
    // SetUnhandledExceptionFilter gets overridden by Kenshi, so we use VEH instead.
    // Filtered to only log crashes in game module or NULL (skips system DLL noise).
    g_vehHandle = AddVectoredExceptionHandler(1, VectoredCrashHandler);

    // ES: ── Cabecera de sesión en CRASH.log ── marca el inicio de cada sesión para agrupar entradas.
    // ── Session header in CRASH.log ──
    // Mark session boundaries so crash analysis can tell which entries belong together.
    {
        FILE* cf = nullptr;
        fopen_s(&cf, "KenshiOnline_CRASH.log", "a");
        if (cf) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(cf, "\n═══ KMP SESSION START %04d-%02d-%02d %02d:%02d:%02d PID=%lu "
                        "gameBase=0x%llX ═══\n",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond,
                    GetCurrentProcessId(),
                    (unsigned long long)g_gameModuleBase);
            fclose(cf);
        }
    }

    // ES: ── Fichero de migas de pan ── se crea al arrancar y se reescribe antes de cada operación
    //     peligrosa; aunque fallen el VEH y atexit (p.ej. __fastfail), muestra el último paso conocido.
    // ── Breadcrumb file for crash diagnosis ──
    // Opened once at startup, written before every dangerous operation, fflush'd.
    // Even if VEH and atexit fail (e.g., __fastfail), the file shows last known step.
    {
        FILE* bcf = nullptr;
        fopen_s(&bcf, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Kenshi\\KenshiOnline_BREADCRUMB.txt", "w");
        if (bcf) {
            fprintf(bcf, "KMP breadcrumb file created — monitoring for crash location\n");
            fflush(bcf);
            fclose(bcf);
        }
    }

    // ES: ── Detección de último recurso ── el "crash silencioso" post-carga no lo captura el VEH (no es
    //     AV ni desbordamiento de pila). Se registran atexit + SetUnhandledExceptionFilter para ver qué
    //     nos mata.
    // ── Last-resort crash detection ──
    // The post-loading "silent crash" isn't caught by VEH (not an AV/stack overflow).
    // Register atexit + SetUnhandledExceptionFilter to detect what kills us.
    atexit([]() {
        OutputDebugStringA("KMP: atexit() fired — process exiting\n");
        char buf[256];
        sprintf_s(buf, "KMP: atexit — LastCreate=#%d, LastTick=#%d step=%d (%s)\n",
                  g_lastCharacterCreateNum, g_tickNumber, g_lastTickStep,
                  g_lastStepName ? g_lastStepName : "?");
        OutputDebugStringA(buf);

        FILE* f = nullptr;
        fopen_s(&f, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Kenshi\\KenshiOnline_CRASH.log", "a");
        if (f) {
            fprintf(f, "\nKMP atexit: LastCreate=#%d, LastTick=#%d step=%d (%s)\n",
                    g_lastCharacterCreateNum, g_tickNumber, g_lastTickStep,
                    g_lastStepName ? g_lastStepName : "?");
            fclose(f);
        }
    });

    // ES: ── Manejador de SIGABRT ── abort() del runtime de C (asserts fallidos, corrupción de heap) no
    //     llama a atexit; este manejador registra el último estado conocido antes de terminar.
    // ── Abort signal handler ──
    // C runtime abort() (e.g., from failed assertions or heap corruption) doesn't
    // call atexit. This handler logs the last known state before termination.
    signal(SIGABRT, [](int) {
        char buf[256];
        sprintf_s(buf, "KMP SIGABRT: LastCreate=#%d, LastTick=#%d step=%d (%s)\n",
                  g_lastCharacterCreateNum, g_tickNumber, g_lastTickStep,
                  g_lastStepName ? g_lastStepName : "?");
        OutputDebugStringA(buf);

        FILE* f = nullptr;
        fopen_s(&f, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Kenshi\\KenshiOnline_CRASH.log", "a");
        if (f) {
            fprintf(f, "\nKMP SIGABRT: LastCreate=#%d, LastTick=#%d step=%d (%s)\n",
                    g_lastCharacterCreateNum, g_tickNumber, g_lastTickStep,
                    g_lastStepName ? g_lastStepName : "?");
            fclose(f);
        }
    });

    // ES: Filtro de excepciones no manejadas — captura las que se escapan tanto del VEH como del SEH.
    //     (Kenshi puede sobrescribirlo después; por eso el VEH es la vía principal.)
    // EN: (Kenshi may override it later; that is why the VEH is the main path.)
    // Unhandled exception filter — catches exceptions that bypass both VEH and SEH
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        uintptr_t rip = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);

        char buf[512];
        sprintf_s(buf,
            "KMP UNHANDLED EXCEPTION: code=0x%08lX RIP=0x%016llX "
            "LastCreate=#%d LastTick=#%d step=%d (%s)\n",
            code, (unsigned long long)rip,
            g_lastCharacterCreateNum, g_tickNumber, g_lastTickStep,
            g_lastStepName ? g_lastStepName : "?");
        OutputDebugStringA(buf);

        // Write to crash log
        FILE* f = nullptr;
        fopen_s(&f, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Kenshi\\KenshiOnline_CRASH.log", "a");
        if (f) {
            fprintf(f, "\n%s", buf);
            auto* ctx = ep->ContextRecord;
            fprintf(f, "  RSP=0x%016llX RBP=0x%016llX\n",
                    (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rbp);
            fprintf(f, "  RAX=0x%016llX RBX=0x%016llX RCX=0x%016llX RDX=0x%016llX\n",
                    (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx,
                    (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx);
            fclose(f);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    });

    OutputDebugStringA("KMP: === Kenshi-Online v0.1.0 Initializing ===\n");
    spdlog::info("=== Kenshi-Online v{}.{}.{} Initializing ===", 0, 1, 0);

    // ES: ── Detección Steam vs GOG ── si steam_api64.dll está cargada es la versión de Steam (GOG no la
    //     tiene). También se registra el tamaño del exe como huella de versión.
    // ── Steam vs GOG Detection ──
    // Check if steam_api64.dll is loaded (Steam version has it, GOG does not).
    {
        HMODULE steamApi = GetModuleHandleA("steam_api64.dll");
        bool isSteam = (steamApi != nullptr);
        m_isSteamVersion = isSteam;

        // Also get executable file size as a version fingerprint
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        HANDLE hFile = CreateFileA(exePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD exeSize = 0;
        if (hFile != INVALID_HANDLE_VALUE) {
            exeSize = GetFileSize(hFile, nullptr);
            CloseHandle(hFile);
        }

        spdlog::info("Platform: {} | exe={} ({} bytes) | module base=0x{:X}",
                      isSteam ? "STEAM" : "GOG", exePath, exeSize, g_gameModuleBase);
        m_nativeHud.LogStep("INIT", isSteam ? "Platform: STEAM" : "Platform: GOG");
    }

    m_nativeHud.LogStep("INIT", "Kenshi Online v0.1.0 starting...");

    // ES: Cargar la configuración del cliente (servidor, nombre, flags como useSyncOrchestrator).
    // EN: Load client config (server, name, flags such as useSyncOrchestrator).
    // Load config
    std::string configPath = ClientConfig::GetDefaultPath();
    m_config.Load(configPath);
    m_nativeHud.LogStep("INIT", "Config loaded");

    // ES: Inicializar offsets de campos del juego (valores de respaldo sacados con Cheat Engine).
    // Initialize game offsets (CE fallbacks)
    game::InitOffsetsFromScanner();

    // ES: Intentar restaurar offsets descubiertos en tiempo de ejecución desde la caché en disco.
    // Try to restore runtime-discovered offsets from cache
    if (game::LoadOffsetCache()) {
        m_nativeHud.LogStep("INIT", "Offsets loaded from cache");
    }
    m_nativeHud.LogStep("INIT", "Game offsets initialized");

    // ES: Inicializar el escáner de patrones (resuelve direcciones de funciones del juego por AOB/strings).
    // Initialize pattern scanner
    m_nativeHud.LogStep("SCAN", "Pattern scanner starting...");
    if (!InitScanner()) {
        m_nativeHud.LogStep("ERR", "Pattern scanner FAILED");
    } else {
        m_nativeHud.LogStep("OK", "Pattern scanner resolved game functions");
    }

    // ES: Inicializar MinHook (librería de hooks). Si falla no se pueden instalar hooks → abortar.
    // Initialize MinHook
    m_nativeHud.LogStep("HOOK", "MinHook initializing...");
    if (!HookManager::Get().Initialize()) {
        m_nativeHud.LogStep("ERR", "MinHook FAILED - cannot install hooks");
        return false;
    }
    m_nativeHud.LogStep("OK", "MinHook ready");

    // ES: El guard de carga empieza en FALSE — solo se activa cuando una ráfaga de CharacterCreate
    //     detecta una carga real (no en el menú principal).
    // Loading guard starts FALSE — only set true when CharacterCreate burst
    // detects actual game loading (not at main menu).
    // m_isLoading = false; // already default
    m_nativeHud.LogStep("INIT", "Loading guard inactive (waiting for game load)");

    // ES: Instalar los hooks del juego (ver InitHooks).
    // Install hooks
    m_nativeHud.LogStep("HOOK", "Installing game hooks...");
    if (!InitHooks()) {
        m_nativeHud.LogStep("WARN", "Some hooks failed to install");
    } else {
        m_nativeHud.LogStep("OK", "All hooks installed");
    }

    // ES: Inicializar la red (ENet).
    // Initialize networking
    m_nativeHud.LogStep("NET", "Network (ENet) initializing...");
    if (!InitNetwork()) {
        m_nativeHud.LogStep("ERR", "Network init FAILED");
    } else {
        m_nativeHud.LogStep("OK", "Network ready (ENet)");
    }

    // ES: Registrar los handlers de mensajes de red.
    // Initialize packet handler
    InitPacketHandler();
    m_nativeHud.LogStep("NET", "Packet handler registered");

    // ES: Callback de SpawnManager: cuando se materializa un personaje remoto, enlaza netId → objeto del
    //     juego en el registro, avisa a PlayerController, lee el nombre y muestra un mensaje de sistema.
    // EN: SpawnManager callback: when a remote character materializes, links netId → game object in the
    //     registry, notifies PlayerController, reads the name and shows a system message.
    // Set up SpawnManager callback
    m_spawnManager.SetOnSpawnedCallback(
        [this](EntityID netId, void* gameObject) {
            m_entityRegistry.SetGameObject(netId, gameObject);
            auto infoCopy = m_entityRegistry.GetInfo(netId);
            PlayerID owner = infoCopy ? infoCopy->ownerPlayerId : 0;
            m_playerController.OnRemoteCharacterSpawned(netId, gameObject, owner);
            game::CharacterAccessor accessor(gameObject);
            std::string charName = accessor.GetName();
            spdlog::info("Core: SpawnManager linked entity {} to game object 0x{:X} (name='{}')",
                         netId, reinterpret_cast<uintptr_t>(gameObject), charName);
            m_overlay.AddSystemMessage("Remote player spawned: " + charName);
        });
    m_nativeHud.LogStep("GAME", "Spawn manager callback set");

    // ES: UI (overlay MyGUI); puede devolver false y hacerse perezosamente cuando exista el device D3D11.
    // EN: UI (MyGUI overlay); may return false and initialize lazily once the D3D11 device exists.
    if (!InitUI()) {
        m_nativeHud.LogStep("WARN", "UI init returned false (lazy init later)");
    }

    m_running = true;

    // ES: Registrar los comandos de chat con barra (/tp, /forcespawn, /sync...).
    // Register slash commands
    CommandRegistry::Get().RegisterBuiltins();
    m_nativeHud.LogStep("CMD", "Slash commands registered");

    // ES: Arrancar el orquestador de tareas de fondo (2 workers).
    // Start background task orchestrator
    m_orchestrator.Start(2);
    m_nativeHud.LogStep("SYS", "Task orchestrator started (2 workers)");

    // ES: Construir el orquestador de sync (EntityResolver, ZoneEngine, PlayerEngine), enlazar los
    //     facilitadores y el depurador de pipeline. m_useSyncOrchestrator elige la rama del tick.
    // EN: m_useSyncOrchestrator picks the tick branch.
    // Construct sync orchestrator (EntityResolver, ZoneEngine, PlayerEngine)
    m_syncOrchestrator = std::make_unique<SyncOrchestrator>(
        m_entityRegistry, m_playerController, m_interpolation,
        m_spawnManager, m_client, m_orchestrator);
    m_useSyncOrchestrator = m_config.useSyncOrchestrator;
    SyncFacilitator::Get().Bind(m_syncOrchestrator.get(), &m_entityRegistry,
                                 &m_interpolation, &m_spawnManager);
    AssetFacilitator::Get().Bind(&m_loadingOrch);
    m_pipelineOrch.Initialize(m_localPlayerId, m_entityRegistry, m_spawnManager,
                               m_loadingOrch, m_client, m_nativeHud);
    m_nativeHud.LogStep("SYS", m_useSyncOrchestrator
        ? "Sync orchestrator ACTIVE (new 7-stage pipeline)"
        : "Sync orchestrator STANDBY (legacy pipeline)");

    // ES: Arrancar el hilo de red (NetworkThreadFunc).
    // Start network thread
    m_networkThread = std::thread(&Core::NetworkThreadFunc, this);
    m_nativeHud.LogStep("NET", "Network thread started");

    m_nativeHud.LogStep("OK", "=== Initialization complete ===");
    m_nativeHud.LogStep("INIT", "Waiting for game to load...");
    m_nativeHud.LogStep("INIT", "Press F1 for menu | Insert to toggle log");

    OutputDebugStringA("KMP: === Kenshi-Online Initialized Successfully ===\n");
    spdlog::info("=== Kenshi-Online Initialized Successfully ===");
    return true;
}

// ES: Envoltorio SEH para los pasos de apagado — dentro de __try no se permiten objetos C++ con
//     destructor. Devuelve false si el paso lanzó una excepción.
// EN: Returns false if the step raised an exception.
// SEH wrapper for shutdown steps — no C++ objects with destructors allowed inside __try
static bool SEH_ShutdownStep(void (*fn)()) {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Declaración adelantada — definida tras OnGameTick en la sección "Staged Pipeline Methods".
//     Repara el puntero de facción de un personaje si está liberado/stale.
// EN: Repairs a character faction pointer if it is freed/stale.
// Forward declaration — defined after OnGameTick in "Staged Pipeline Methods" section
static int SEH_ValidateEntityFaction(void* gameObj, uintptr_t goodFaction,
                                      uintptr_t modBase, size_t modSize);

// ES: Tick del orquestador de sync protegido con SEH. Si el juego peta dentro de lecturas/escrituras
//     de posición (p.ej. personajes ya liberados), se captura en vez de matar el proceso.
// SEH-protected sync orchestrator tick. If the game crashes inside
// position reads/writes (e.g., freed character objects), catch it
// instead of killing the process.
static bool SEH_SyncOrchestratorTick(SyncOrchestrator* orch, float dt) {
    __try {
        orch->Tick(dt);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_syncCrashCount = 0;
        if (++s_syncCrashCount <= 10) {
            char buf[128];
            sprintf_s(buf, "KMP: SEH_SyncOrchestratorTick CRASHED (count=%d)\n", s_syncCrashCount);
            OutputDebugStringA(buf);
        }
        return false;
    }
}

// ES: Update de la interpolación protegido con SEH.
// EN: SEH-protected interpolation update.
static bool SEH_InterpolationUpdate(Interpolation* interp, float dt) {
    __try {
        interp->Update(dt);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Ejecuta UN comando de la cola con SEH propio — un puntero malo en una lambda no debe
// tirar el resto de la ráfaga ni el resto del tick.
// EN: Runs ONE queued command with its own SEH — a bad pointer inside a lambda must not take down
//     the rest of the burst nor the rest of the tick.
static bool SEH_RunGameCommand(kmp::GameCommand* cmd) {
    __try {
        cmd->execute();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Ejecutor del sondeo de offsets protegido con SEH: recorre personajes para encontrar uno del
//     jugador (facción == earlyFaction) y un NPC, y lanza el prober unificado con ambos.
//     Sin objetos C++ con destructor — solo locales POD.
// SEH-protected offset prober runner.
// Iterates characters to find a player and NPC, then runs the unified prober.
// No C++ objects with destructors — uses only POD locals.
static bool SEH_RunOffsetProber(uintptr_t earlyFaction) {
    __try {
        game::CharacterIterator probeIter;
        uintptr_t probePlayer = 0, probeNpc = 0;
        while (probeIter.HasNext() && (probePlayer == 0 || probeNpc == 0)) {
            game::CharacterAccessor ch = probeIter.Next();
            if (!ch.IsValid()) continue;
            uintptr_t fp = ch.GetFactionPtr();
            if (fp == earlyFaction && probePlayer == 0)
                probePlayer = ch.GetPtr();
            else if (fp != earlyFaction && probeNpc == 0)
                probeNpc = ch.GetPtr();
        }
        if (probePlayer != 0) {
            game::RunOffsetProber(probePlayer, probeNpc);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: SEH_RunOffsetProber CRASHED\n");
        return false;
    }
}

// ES: Tick del LoadingOrchestrator (gating de spawns por recursos cargados) protegido con SEH.
// EN: SEH-protected LoadingOrchestrator tick (gates spawns on loaded resources).
static void SEH_LoadingOrchTick(LoadingOrchestrator* orch) {
    __try {
        orch->Tick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_count = 0;
        if (++s_count <= 5) {
            char buf[128];
            sprintf_s(buf, "KMP: SEH_LoadingOrchTick CRASHED (count=%d)\n", s_count);
            OutputDebugStringA(buf);
        }
    }
}

// ES: Apagado ordenado del mod (lo llama dllStopPlugin / DllMain). Para orquestadores y el hilo de
//     red, desconecta, desinstala hooks inline y MinHook, cierra la UI (cada paso con SEH), quita el
//     VEH y guarda la config en una ruta por instancia.
// EN: Orderly mod shutdown (called by dllStopPlugin / DllMain). Stops orchestrators and the network
//     thread, disconnects, uninstalls inline and MinHook hooks, closes the UI (each step under SEH),
//     removes the VEH and saves the config to a per-instance path.
void Core::Shutdown() {
    spdlog::info("Kenshi-Online shutting down...");

    m_running = false;
    m_connected = false;

    // ES: Cada paso de apagado va protegido con SEH por separado — Kenshi puede haber liberado ya los
    //     widgets de MyGUI, provocando AV en setVisible/setCaption durante el cierre.
    // Each shutdown step is SEH-protected individually — MyGUI widgets may already be
    // freed by Kenshi, causing AVs in setVisible/setCaption calls during teardown.
    m_pipelineOrch.Shutdown();
    AssetFacilitator::Get().Unbind();
    SyncFacilitator::Get().Unbind();
    if (m_syncOrchestrator) {
        m_syncOrchestrator->Shutdown();
    }

    m_orchestrator.Stop();

    if (m_networkThread.joinable()) {
        m_networkThread.join();
    }

    m_client.Disconnect();

    // ES: Desinstalar los hooks inline antes que HookManager (que solo gestiona los de MinHook).
    // Uninstall inline hooks before HookManager (which only handles MinHook hooks)
    squad_spawn_hooks::Uninstall();
    char_tracker_hooks::Uninstall();

    // ES: Estos tocan MyGUI — los que más probabilidad tienen de petar durante el cierre de Kenshi.
    // These touch MyGUI — most likely to crash during Kenshi's own shutdown
    if (!SEH_ShutdownStep([](){ Core::Get().GetNativeHud().Shutdown(); })) {
        OutputDebugStringA("KMP: SEH — NativeHud::Shutdown crashed\n");
    }
    if (!SEH_ShutdownStep([](){ Core::Get().GetOverlay().Shutdown(); })) {
        OutputDebugStringA("KMP: SEH — Overlay::Shutdown crashed\n");
    }
    if (!SEH_ShutdownStep([](){ HookManager::Get().Shutdown(); })) {
        OutputDebugStringA("KMP: SEH — HookManager::Shutdown crashed\n");
    }

    // ES: Quitar el manejador VEH.
    // Remove vectored exception handler
    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }

    // ES: Guardar la config en ruta por PID (para no pisar la config de otra instancia).
    // Save config to PID-specific path (avoids stomping another instance's config)
    m_config.Save(ClientConfig::GetInstancePath());

    spdlog::info("Kenshi-Online shutdown complete");
}

// ES: Descubre las funciones y globales del juego. Ejecuta el escáner legacy y el
//     PatternOrchestrator de 7 fases (ambos rellenan m_gameFuncs), valida los singletons
//     PlayerBase/GameWorld (RVAs fijos solo válidos en GOG 1.0.68) y analiza los prólogos de las
//     funciones hookeadas para anular las que no cuadran con la firma esperada.
//     Un patrón AOB es una firma de bytes con comodines que se busca en el exe para localizar una
//     función sin depender de su dirección fija.
// EN: Discovers the game functions and globals. Runs the legacy scanner and the 7-phase
//     PatternOrchestrator (both fill m_gameFuncs), validates the PlayerBase/GameWorld singletons
//     (hard-coded RVAs only valid on GOG 1.0.68) and analyzes the prologues of the hooked functions
//     to null out those that do not match the expected signature.
//     An AOB pattern is a byte signature with wildcards searched in the exe to find a function
//     without depending on its fixed address.
bool Core::InitScanner() {
    // ES: Escáner legacy (se mantiene por compatibilidad).
    // ══════════════════════════════════════════════════════════════
    //  LEGACY SCANNER (kept for backward compatibility)
    // ══════════════════════════════════════════════════════════════
    if (!m_scanner.Init(nullptr)) {
        spdlog::error("Failed to init scanner for main executable");
        return false;
    }

    // ES: PATTERN ORCHESTRATOR — descubrimiento multi-fase: 1) enumeración de .pdata (todas las funciones
    //     del exe), 2) strings + referencias cruzadas, 3) vtables + jerarquía RTTI, 4) escaneo SIMD por
    //     lotes, 5) fallback por xref de strings, 6) grafo de llamadas + propagación de etiquetas,
    //     7) validación de punteros globales.
    // ══════════════════════════════════════════════════════════════
    //  PATTERN ORCHESTRATOR — Enhanced multi-phase discovery
    // ══════════════════════════════════════════════════════════════
    //  7-phase pipeline:
    //    1. .pdata enumeration (every function in the exe)
    //    2. String discovery + cross-reference analysis
    //    3. VTable scanning + RTTI class hierarchy mapping
    //    4. SIMD batch pattern scan (all patterns in one pass)
    //    5. String xref fallback (for patterns that failed)
    //    6. Call graph analysis + label propagation
    //    7. Global pointer validation
    // ══════════════════════════════════════════════════════════════

    OrchestratorConfig orchConfig;
    orchConfig.enablePData     = true;
    orchConfig.enableStrings   = true;
    orchConfig.enableVTables   = true;
    orchConfig.enableCallGraph = true;
    orchConfig.enableBatchScan = true;
    orchConfig.enableLabelPropagation = true;
    orchConfig.stringMinLength = 4;
    orchConfig.callGraphDepth  = 2;
    orchConfig.fullCallGraph   = false; // Targeted graph for performance
    orchConfig.scanWideStrings = false;

    if (m_patternOrchestrator.Init(nullptr, orchConfig)) {
        // ES: Registrar todos los patrones integrados (rellenan GameFunctions vía punteros destino) y
        //     ejecutar la pipeline completa de 7 fases.
        // Register all built-in patterns (populates GameFunctions via target pointers)
        m_patternOrchestrator.RegisterBuiltinPatterns(m_gameFuncs);

        // Run the full 7-phase pipeline
        auto report = m_patternOrchestrator.Run();

        m_nativeHud.LogStep("SCAN",
            "Orchestrator: " + std::to_string(report.totalResolved) + "/" +
            std::to_string(report.totalEntries) + " resolved (" +
            std::to_string(report.pdataFunctions) + " .pdata funcs, " +
            std::to_string(report.vtablesFound) + " vtables, " +
            std::to_string(report.labeledFunctions) + " labeled)");
    }

    // ES: Ejecutar siempre también el escáner legacy — garantiza el descubrimiento de PlayerBase y el
    //     camino que se sabe que funciona.
    // Always run legacy scanner too — ensures PlayerBase discovery + proven-working path
    {
        bool resolved = ResolveGameFunctions(m_scanner, m_gameFuncs);
        if (!resolved) {
            spdlog::warn("Game functions minimally resolved: false - some features may not work");
        }
    }

    // ES: ── Validación de singletons en Steam ── Los RVAs fijos (PlayerBase=0x01AC8A90,
    //     GameWorld=0x02133040; RVA = dirección relativa a la base de kenshi_x64.exe) son solo de GOG
    //     1.0.68. En Steam esas direcciones contienen otros datos y desreferenciarlas da punteros basura
    //     que tumban CharacterIterator, SpawnManager, etc. Se valida que el valor parezca un puntero de
    //     heap real (fuera del módulo y desreferenciable); si no, se limpia a 0.
    // ── Steam singleton validation ──
    // Hardcoded RVAs (PlayerBase=0x01AC8A90, GameWorld=0x02133040) are GOG 1.0.68 only.
    // On Steam, these addresses contain unrelated data — dereferencing reads garbage
    // pointers that crash downstream systems (CharacterIterator, SpawnManager, etc.).
    // Validate by reading the value and checking it looks like a real heap pointer.
    {
        uintptr_t base = m_scanner.GetBase();
        size_t size = m_scanner.GetSize();
        auto validateSingleton = [base, size](uintptr_t addr, const char* name) -> bool {
            if (addr == 0) return true; // Not resolved, nothing to validate
            uintptr_t val = 0;
            if (!Memory::Read(addr, val)) {
                spdlog::warn("Core: {} at 0x{:X} — unreadable, clearing", name, addr);
                return false;
            }
            // Must be in valid userspace heap range and NOT inside the module image
            if (val == 0) return true; // NULL is OK (game not loaded yet)
            if (val < 0x10000 || val >= 0x00007FFFFFFFFFFF) {
                spdlog::warn("Core: {} at 0x{:X} reads garbage 0x{:X} — clearing (Steam offset mismatch)",
                             name, addr, val);
                return false;
            }
            if (val >= base && val < base + size) {
                spdlog::warn("Core: {} at 0x{:X} reads module-internal ptr 0x{:X} — clearing",
                             name, addr, val);
                return false;
            }
            // Double-dereference: a real singleton should point to a readable object
            uintptr_t check = 0;
            if (!Memory::Read(val, check)) {
                spdlog::warn("Core: {} at 0x{:X} -> 0x{:X} not dereferenceable — clearing",
                             name, addr, val);
                return false;
            }
            return true;
        };

        if (!validateSingleton(m_gameFuncs.PlayerBase, "PlayerBase")) {
            m_gameFuncs.PlayerBase = 0;
        }
        // GameWorld 1.0.68 = instancia embebida: usar validador que acepta instancia-directa.
        // No usar validateSingleton (espera puntero heap y rechazaria la instancia). Solo
        // limpiamos si addr != 0 y el validador especifico falla (preserva el NULL temprano).
        // EN: GameWorld 1.0.68 = embedded instance: use the validator that accepts a direct instance.
        //     Do not use validateSingleton (it expects a heap pointer and would reject the instance). Only
        //     clear it if addr != 0 and the specific validator fails (keeps the early NULL). A first qword
        //     of 0 means "not loaded yet"; anything else that is not a plausible GameWorld is cleared.
        if (m_gameFuncs.GameWorldSingleton != 0 &&
            !ValidateGameWorldGlobal(m_gameFuncs.GameWorldSingleton, base, size)) {
            // Distinguir "aun no cargado" (primer qword == 0) de "basura real".
            uintptr_t fw = 0;
            Memory::Read(m_gameFuncs.GameWorldSingleton, fw);
            if (fw != 0) { // hay algo y no es GameWorld plausible -> limpiar
                spdlog::warn("Core: GameWorldSingleton at 0x{:X} not a GameWorld instance (first=0x{:X}) — clearing",
                             m_gameFuncs.GameWorldSingleton, fw);
                m_gameFuncs.GameWorldSingleton = 0;
            }
        }

        if (m_isSteamVersion && m_gameFuncs.PlayerBase == 0 && m_gameFuncs.GameWorldSingleton == 0) {
            spdlog::info("Core: Steam version — singletons cleared (will use loading cache + entity_hooks fallback)");
            m_nativeHud.LogStep("INFO", "Steam: using entity_hooks fallback (no hardcoded singletons)");
        }
    }

    // ES: ── Análisis de firmas de funciones ── analiza los prólogos de todas las funciones hookeadas para
    //     validar nuestras firmas (número de parámetros del typedef del hook). Solo se anulan las que son
    //     peligrosas con firma errónea (CharacterMoveTo, BuildingPlace, SaveGame, LoadGame).
    // ── Function Signature Analysis ──
    // Analyze prologues of all hooked functions to validate our signatures.
    {
        std::vector<FunctionSignature> sigs;

        struct HookSigCheck {
            const char* name;
            void* address;
            int hookParamCount; // params in our hook typedef
        };

        HookSigCheck checks[] = {
            {"CharacterSpawn",       m_gameFuncs.CharacterSpawn,       2}, // factory, templateData
            {"CharacterDestroy",     m_gameFuncs.CharacterDestroy,     1}, // character
            {"CharacterSetPosition", m_gameFuncs.CharacterSetPosition, 2}, // character, Vec3*
            {"CharacterMoveTo",      m_gameFuncs.CharacterMoveTo,      0}, // MID-FUNCTION — do not hook/call
            {"ApplyDamage",          m_gameFuncs.ApplyDamage,          6}, // target, attacker, bodyPart, cut, blunt, pierce
            {"CharacterDeath",       m_gameFuncs.CharacterDeath,       2}, // character, killer
            {"ZoneLoad",             m_gameFuncs.ZoneLoad,             3}, // zoneMgr, zoneX, zoneY
            {"ZoneUnload",           m_gameFuncs.ZoneUnload,           3}, // zoneMgr, zoneX, zoneY
            {"BuildingPlace",        m_gameFuncs.BuildingPlace,        5}, // world, building, x, y, z
            {"SaveGame",             m_gameFuncs.SaveGame,             2}, // saveManager, saveName
            {"LoadGame",             m_gameFuncs.LoadGame,             2}, // saveManager, saveName
        };

        // ES: Mapa nombre → puntero en m_gameFuncs para poder anular las que no cuadren.
        // Map from check name to m_gameFuncs pointer so we can null mismatches
        std::unordered_map<std::string, void**> funcPtrs = {
            {"CharacterMoveTo",      &m_gameFuncs.CharacterMoveTo},
            {"BuildingPlace",        &m_gameFuncs.BuildingPlace},
            {"SaveGame",             &m_gameFuncs.SaveGame},
            {"LoadGame",             &m_gameFuncs.LoadGame},
        };

        for (auto& check : checks) {
            if (!check.address) continue;
            auto sig = FunctionAnalyzer::Analyze(
                reinterpret_cast<uintptr_t>(check.address), check.name);
            if (sig.IsValid()) {
                bool ok = FunctionAnalyzer::ValidateSignature(sig, check.hookParamCount);
                if (!ok) {
                    // Only null functions that are known to be dangerous with wrong signatures
                    auto it = funcPtrs.find(check.name);
                    if (it != funcPtrs.end() && it->second) {
                        *(it->second) = nullptr;
                        spdlog::warn("Core: SIGNATURE MISMATCH for '{}' — hook expects {} params, analysis suggests ~{} — NULLED",
                                     check.name, check.hookParamCount, sig.estimatedParams);
                    } else {
                        // ES: Las funciones con prólogo MovRaxRsp confunden la detección de parámetros — solo avisar.
                        // MovRaxRsp functions confuse param detection — warn but don't disable
                        spdlog::warn("Core: Signature analysis for '{}' — hook expects {} params, analysis suggests ~{} (MovRaxRsp may confuse analyzer — keeping)",
                                     check.name, check.hookParamCount, sig.estimatedParams);
                    }
                }
                sigs.push_back(std::move(sig));
            }
        }

        FunctionAnalyzer::LogAnalysis(sigs);
    }

    // ES: Puentear PlayerBase al módulo game_character (lo usa CharacterIterator).
    // Bridge PlayerBase to the game_character module
    if (m_gameFuncs.PlayerBase != 0) {
        game::SetResolvedPlayerBase(m_gameFuncs.PlayerBase);
        spdlog::info("Core: PlayerBase bridged to game_character at 0x{:X}", m_gameFuncs.PlayerBase);
    }

    // ES: Puentear la función CharacterSetPosition del juego al módulo game_character.
    // Bridge CharacterSetPosition function to game_character module
    if (m_gameFuncs.CharacterSetPosition) {
        game::SetGameSetPositionFn(m_gameFuncs.CharacterSetPosition);
        spdlog::info("Core: SetPosition function bridged at 0x{:X}",
                     reinterpret_cast<uintptr_t>(m_gameFuncs.CharacterSetPosition));
    }

    // Bridge el setter OFICIAL de pausa (GameWorld::setPaused, 0x787D40) al módulo game.
    // GameWorldAccessor::SetPaused lo usará para despausar de forma COMPLETA (simulación +
    // caches de subsistemas + HUD), arreglando la "pausa fantasma" que bloqueaba órdenes.
    // EN: Bridge the OFFICIAL pause setter (GameWorld::setPaused, 0x787D40) into the game module.
    //     GameWorldAccessor::SetPaused uses it to unpause COMPLETELY (simulation + subsystem caches +
    //     HUD), fixing the "ghost pause" that blocked orders. Without it, a raw byte write is used.
    if (m_gameFuncs.SetPaused) {
        game::SetGameSetPausedFn(m_gameFuncs.SetPaused);
        spdlog::info("Core: SetPaused (setter oficial) bridged at 0x{:X}",
                     reinterpret_cast<uintptr_t>(m_gameFuncs.SetPaused));
    } else {
        spdlog::warn("Core: SetPaused (setter oficial) NO resuelto — la despausa usará "
                     "write crudo del byte (puede persistir la 'pausa fantasma')");
    }

    // ES: Devuelve true si están resueltas las funciones mínimas imprescindibles.
    // EN: Returns true if the minimum required functions are resolved.
    return m_gameFuncs.IsMinimallyResolved();
}

// ES: Declaración adelantada — definida más abajo, junto a los envoltorios SEH de OnGameLoaded.
// Forward declaration — defined below OnGameLoaded
static bool SEH_InstallEntityHooks();

// ES: Instala los hooks del juego al arrancar (MinHook desvía la función del juego a la nuestra y
//     guarda un trampolín para llamar a la original). Present (D3D11) siempre; CharacterCreate se
//     instala pero queda DESACTIVADO hasta conectar; combate, squad spawn, char tracker, inventario,
//     facción, tiempo e IA se instalan pronto con sus propios guards. Devuelve false si falla alguno
//     crítico (Present o CharacterCreate).
// EN: Installs the game hooks at startup (MinHook detours the game function to ours and keeps a
//     trampoline to call the original). Present (D3D11) always; CharacterCreate is installed but
//     stays DISABLED until connecting; combat, squad spawn, char tracker, inventory, faction, time
//     and AI are installed early with their own guards. Returns false if a critical one fails
//     (Present or CharacterCreate).
bool Core::InitHooks() {
    bool allOk = true;

    // ES: Renderizado ImGui ELIMINADO (conflicto Ogre3D/DX11). El hook de Present solo se usa para
    //     HWND/WndProc + OnGameTick. La UI es el menú nativo de MyGUI.
    // ══════════════════════════════════════════════════════════════
    // ImGui rendering REMOVED — Ogre3D/DX11 conflict.
    // Using Present hook for HWND/WndProc + OnGameTick only.
    // UI is native MyGUI menu.
    // ══════════════════════════════════════════════════════════════

    // ES: Hook de Present de D3D11 (entrada por WndProc + tick de respaldo, SIN render de ImGui).
    // D3D11 Present hook (WndProc input + OnGameTick fallback, NO ImGui rendering)
    m_nativeHud.LogStep("HOOK", "D3D11 Present hook...");
    if (!render_hooks::Install()) {
        m_nativeHud.LogStep("ERR", "Present hook FAILED");
        allOk = false;
    } else {
        m_nativeHud.LogStep("OK", "Present hook installed (WndProc + frame tick)");
    }

    // ES: Hooks de entrada: ahora los gestiona el WndProc de render_hooks (input_hooks ya no hace falta).
    // Input hooks: handled by WndProc in render_hooks now
    // input_hooks not needed

    // ES: El hook de CharacterCreate se INSTALA pero se DESACTIVA en el acto. No puede estar activo
    //     durante la carga: las 130+ llamadas seguidas a CharacterCreate a través del detour desnudo
    //     MovRaxRsp corrompen el heap. Solo se activa al conectar a un servidor (ResumeForNetwork).
    //     El descubrimiento de personajes durante/tras la carga usa CharacterIterator.
    // ═══════════════════════════════════════════════════════════════════
    // CharacterCreate hook is INSTALLED but DISABLED immediately.
    // It must NOT be active during game loading — the 130+ rapid-fire
    // CharacterCreate calls through the MovRaxRsp naked detour corrupt
    // the heap. The hook is only enabled when connecting to a multiplayer
    // server (via ResumeForNetwork). Character discovery during/after
    // loading uses CharacterIterator instead.
    // ═══════════════════════════════════════════════════════════════════
    if (m_gameFuncs.CharacterSpawn) {
        m_nativeHud.LogStep("HOOK", "Entity hooks (CharacterCreate)...");
        if (SEH_InstallEntityHooks()) {
            m_nativeHud.LogStep("OK", "CharacterCreate installed (captures 2, then bypasses)");
        } else {
            m_nativeHud.LogStep("ERR", "CharacterCreate hook FAILED");
            allOk = false;
        }
    } else {
        m_nativeHud.LogStep("ERR", "CharacterSpawn pattern NOT FOUND");
    }

    // ES: Hooks de combate, inventario y facción instalados PRONTO. Se disparan por eventos sueltos (no
    //     en ráfagas de carga), así que el wrapper MovRaxRsp los aguanta. Cada hook tiene SEH y comprueba
    //     IsConnected()/guards de carga antes de enviar paquetes. Antes se diferían a OnGameLoaded, que a
    //     menudo no llegaba a dispararse por un bug de timing en la detección del hueco de carga.
    // ═══════════════════════════════════════════════════════════════════
    // Combat, Inventory, and Faction hooks installed EARLY.
    // These fire on individual game events (not loading bursts), so the
    // MovRaxRsp wrapper handles them fine. Each hook has SEH protection
    // and checks IsConnected()/loading guards before sending packets.
    // Previously deferred to OnGameLoaded which often never fired due
    // to a timing bug in the loading gap detection during Startup phase.
    // ═══════════════════════════════════════════════════════════════════

    // ES: Activar los guards de carga para que los hooks no envíen paquetes mientras carga la partida.
    // Set loading guards so hooks don't send packets during save load
    inventory_hooks::SetLoading(true);
    faction_hooks::SetLoading(true);

    // ES: Hooks de combate (ApplyDamage, CharacterDeath, CharacterKO).
    // Combat hooks (ApplyDamage, CharacterDeath, CharacterKO)
    if (m_gameFuncs.ApplyDamage) {
        m_nativeHud.LogStep("HOOK", "Combat hooks...");
        if (combat_hooks::Install()) {
            m_nativeHud.LogStep("OK", "Combat hooks installed");
        } else {
            m_nativeHud.LogStep("WARN", "Combat hooks FAILED");
        }
    } else {
        m_nativeHud.LogStep("WARN", "ApplyDamage not resolved — combat hooks skipped");
    }

    // ES: Hooks de bypass del spawn de escuadrón (para spawnear remotos de forma fiable).
    // Squad spawn bypass hooks (for reliable remote character spawning)
    {
        m_nativeHud.LogStep("HOOK", "Squad spawn bypass...");
        if (squad_spawn_hooks::Install()) {
            m_nativeHud.LogStep("OK", "Squad spawn bypass installed");
        } else {
            m_nativeHud.LogStep("WARN", "Squad spawn bypass not available (pattern not found)");
        }
    }

    // ES: Hooks del rastreador de personajes (update de animación — rastrea todos los chars por nombre).
    // Character tracker hooks (animation update — tracks all chars by name)
    {
        m_nativeHud.LogStep("HOOK", "Character tracker...");
        if (char_tracker_hooks::Install()) {
            m_nativeHud.LogStep("OK", "Character tracker installed");
        } else {
            m_nativeHud.LogStep("WARN", "Character tracker not available (pattern not found)");
        }
    }

    // ES: Hooks de inventario (ItemPickup, ItemDrop, BuyItem).
    // Inventory hooks (ItemPickup, ItemDrop, BuyItem)
    if (m_gameFuncs.ItemPickup) {
        m_nativeHud.LogStep("HOOK", "Inventory hooks...");
        if (inventory_hooks::Install()) {
            m_nativeHud.LogStep("OK", "Inventory hooks installed");
        } else {
            m_nativeHud.LogStep("WARN", "Inventory hooks FAILED");
        }
    } else {
        m_nativeHud.LogStep("WARN", "ItemPickup not resolved — inventory hooks skipped");
    }

    // ES: Hooks de facción (FactionRelation).
    // Faction hooks (FactionRelation)
    if (m_gameFuncs.FactionRelation) {
        m_nativeHud.LogStep("HOOK", "Faction hooks...");
        if (faction_hooks::Install()) {
            m_nativeHud.LogStep("OK", "Faction hooks installed");
        } else {
            m_nativeHud.LogStep("WARN", "Faction hooks FAILED");
        }
    } else {
        m_nativeHud.LogStep("WARN", "FactionRelation not resolved — faction hooks skipped");
    }

    // ES: Hooks de tiempo — mueven OnGameTick (esencial para el bucle del juego).
    // Time hooks — drives OnGameTick (essential for game loop)
    if (m_gameFuncs.TimeUpdate) {
        m_nativeHud.LogStep("HOOK", "Time hooks...");
        if (time_hooks::Install()) {
            m_nativeHud.LogStep("OK", "Time hooks installed");
        } else {
            m_nativeHud.LogStep("WARN", "Time hooks FAILED");
        }
    }

    // ES: Hooks de IA (AICreate + AIPackages).
    // AI hooks (AICreate + AIPackages)
    if (m_gameFuncs.AICreate) {
        m_nativeHud.LogStep("HOOK", "AI hooks...");
        if (ai_hooks::Install()) {
            m_nativeHud.LogStep("OK", "AI hooks installed");
        } else {
            m_nativeHud.LogStep("WARN", "AI hooks FAILED");
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // [AUDITORÍA 12-jul] world_hooks::Install() NO se llama a propósito
    // (todavía). NO basta con añadir la llamada aquí:
    //   - ZoneUnload (RVA 0x2EF1F0) tiene prólogo `mov rax,rsp` (48 8B C4,
    //     verificado en logs) → InstallAt lo dejaría en BYPASS permanente
    //     (Phase 6) y NADIE llama Enable("ZoneUnload") en todo el proyecto,
    //     así que la limpieza de entidades al descargar zona seguiría sin
    //     correr igualmente.
    //   - ZoneLoad (RVA 0x377710) tiene prólogo limpio (40 55 56 57...) →
    //     quedaría ACTIVO desde el arranque, disparando en cada carga de
    //     zona durante el loading, sin verificación en vivo previa.
    // Activarlo requiere su propia sesión: Install() + Enable() de ambos
    // en OnGameLoaded + verificación en vivo del trampoline de ZoneUnload
    // (mismo riesgo de crash que ItemDrop/BuyItem). Mientras tanto,
    // ProcessDeferredZoneEvents() en OnGameTick drena un ring vacío (inofensivo).
    // Nota: building_hooks::Install() está en la misma situación (sin call site).
    // ═══════════════════════════════════════════════════════════════════
    // EN: [AUDIT 12-Jul] world_hooks::Install() is deliberately NOT called (yet). Adding the call here
    //     is not enough:
    //       - ZoneUnload (RVA 0x2EF1F0) has a `mov rax,rsp` prologue (48 8B C4, verified in logs) →
    //         InstallAt would leave it in permanent BYPASS (Phase 6) and NOBODY calls
    //         Enable("ZoneUnload") anywhere in the project, so entity cleanup on zone unload would
    //         still not run.
    //       - ZoneLoad (RVA 0x377710) has a clean prologue (40 55 56 57...) → it would be ACTIVE from
    //         startup, firing on every zone load during loading, without prior live verification.
    //     Enabling it needs its own session: Install() + Enable() of both in OnGameLoaded + live
    //     verification of the ZoneUnload trampoline (same crash risk as ItemDrop/BuyItem). Meanwhile,
    //     ProcessDeferredZoneEvents() in OnGameTick drains an empty ring (harmless).
    //     Note: building_hooks::Install() is in the same situation (no call site).

    m_nativeHud.LogStep("OK", "All hooks installed");

    return allOk;
}

// ES: Inicializa el cliente de red ENet.
// EN: Initializes the ENet network client.
bool Core::InitNetwork() {
    return m_client.Initialize();
}

// ES: El overlay se inicializa de forma perezosa cuando existe el device D3D11 (en el hook de Present).
bool Core::InitUI() {
    // Overlay is initialized lazily when the D3D11 device is available
    // (happens in the Present hook)
    return true;
}

// ES: ── Envoltorios SEH para los sub-pasos de OnGameLoaded (en __try no se permiten objetos C++) ──
//     Instalan hooks de entidad, notifican a PlayerController, reintentan el descubrimiento de
//     globales y escanean el heap de GameData; si algo peta devuelven false en vez de tumbar el juego.
// EN: They install entity hooks, notify PlayerController, retry global discovery and scan the
//     GameData heap; if something crashes they return false instead of killing the game.
// ── SEH wrappers for OnGameLoaded sub-steps (no C++ objects allowed in __try) ──
static bool SEH_InstallEntityHooks() {
    __try {
        return entity_hooks::Install();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: SEH CRASH in entity_hooks::Install()\n");
        return false;
    }
}

static void SEH_PlayerControllerOnGameWorldLoaded(PlayerController& pc) {
    __try {
        pc.OnGameWorldLoaded();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: SEH CRASH in PlayerController::OnGameWorldLoaded()\n");
    }
}

static bool SEH_RetryGlobalDiscovery(PatternScanner& scanner, GameFunctions& funcs) {
    __try {
        return RetryGlobalDiscovery(scanner, funcs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: SEH CRASH in RetryGlobalDiscovery()\n");
        return false;
    }
}

static void SEH_ScanGameDataHeap(SpawnManager& sm) {
    __try {
        sm.ScanGameDataHeap();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: SEH CRASH in ScanGameDataHeap()\n");
    }
}

// ES: DESCUBRIMIENTO EN TIEMPO DE EJECUCIÓN POR VTABLE. Cuando fallan el escaneo de patrones y el
//     fallback por strings (habitual en Steam), se sacan punteros a funciones críticas de las vtables
//     de objetos vivos del juego.
//     Investigación con CT (tabla de Cheat Engine): slot 2 de la vtable de activePlatoon (offset 0x10)
//     = addMember(character*). Cadena: character → GetSquadPtr() → platoon → +0x1D8 → activePlatoon
//     → vtable+0x10, o bien character+0x658 → activePlatoon → vtable+0x10.
// ═══════════════════════════════════════════════════════════════════════════
//  VTABLE-BASED RUNTIME DISCOVERY
// ═══════════════════════════════════════════════════════════════════════════
// When pattern scan + string fallback fail (common on Steam), discover
// critical function pointers from live game objects' vtables.
//
// CT research: activePlatoon vtable slot 2 (offset 0x10) = addMember(character*)
// Chain: character → GetSquadPtr() → platoon → +0x1D8 → activePlatoon → vtable+0x10
// Or:    character+0x658 → activePlatoon → vtable+0x10

// ES: true cuando ya se descubrió SquadAddMember por vtable (evita repetir).
// EN: true once SquadAddMember has been discovered via vtable (avoids repeating).
static bool s_squadAddMemberDiscovered = false;

// ES: Lectura de un puntero protegida con SEH — los objetos del juego pueden estar liberados.
//     Devuelve 0 si la lectura falla.
// EN: Returns 0 if the read fails.
// SEH-protected pointer chain read — game objects may be freed or invalid
static uintptr_t SEH_ReadPtr(uintptr_t addr) {
    __try {
        return *reinterpret_cast<uintptr_t*>(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ES: Lee el puntero de función del slot slotOffset de la vtable de un objeto (con SEH).
//     Devuelve 0 si la vtable no es un puntero plausible o si la lectura falla.
// EN: Reads the function pointer at slotOffset of an object vtable (under SEH).
//     Returns 0 if the vtable is not a plausible pointer or the read fails.
static uintptr_t SEH_ReadVTableSlot(uintptr_t objectPtr, int slotOffset) {
    __try {
        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(objectPtr);
        if (vtable < 0x10000 || vtable >= 0x00007FFFFFFFFFFF) return 0;
        uintptr_t funcPtr = *reinterpret_cast<uintptr_t*>(vtable + slotOffset);
        return funcPtr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ES: Valida que un candidato sea la entrada real de una función del módulo: dentro de la imagen,
//     inicio exacto según .pdata (RtlLookupFunctionEntry) y tamaño razonable (>= 32 bytes).
// Validate that a candidate function pointer is a real function entry in the module
static bool ValidateVTableCandidate(uintptr_t funcAddr, uintptr_t moduleBase, size_t moduleSize,
                                     const char* source) {
    if (funcAddr == 0) return false;

    // Must be within the module image
    if (funcAddr < moduleBase || funcAddr >= moduleBase + moduleSize) {
        spdlog::debug("VTableDiscovery: {} = 0x{:X} — outside module", source, funcAddr);
        return false;
    }

    // Must be a real function entry per .pdata
    DWORD64 imageBase = 0;
    auto* rtFunc = RtlLookupFunctionEntry(static_cast<DWORD64>(funcAddr), &imageBase, nullptr);
    if (rtFunc) {
        uintptr_t funcStart = static_cast<uintptr_t>(imageBase) + rtFunc->BeginAddress;
        if (funcStart != funcAddr) {
            spdlog::debug("VTableDiscovery: {} = 0x{:X} is +0x{:X} into 0x{:X} — not entry",
                          source, funcAddr, funcAddr - funcStart, funcStart);
            return false;
        }
    }

    // Function size should be reasonable for an addMember (>= 32 bytes)
    if (rtFunc) {
        uint32_t funcSize = rtFunc->EndAddress - rtFunc->BeginAddress;
        if (funcSize < 32) {
            spdlog::debug("VTableDiscovery: {} = 0x{:X} — too small ({} bytes)", source, funcAddr, funcSize);
            return false;
        }
    }

    return true;
}

// ES: Descubre SquadAddMember (activePlatoon::addMember) desde las vtables de objetos vivos, probando
//     varias cadenas a partir del personaje primario. Si lo encuentra instala squad_hooks (o al menos
//     deja el puntero crudo usable). Devuelve false si aún no se pudo (se reintenta en otro tick).
// EN: Returns false if it could not yet (retried on a later tick).
// Discover SquadAddMember from live game objects' vtables.
// Tries multiple approaches to find the activePlatoon::addMember function.
static bool TryDiscoverSquadAddMemberFromVTable(GameFunctions& funcs,
                                                 uintptr_t moduleBase,
                                                 size_t moduleSize) {
    if (funcs.SquadAddMember != nullptr) return true; // Already have it
    if (s_squadAddMemberDiscovered) return false; // Already tried all approaches

    auto& core = Core::Get();
    void* primaryChar = core.GetPlayerController().GetPrimaryCharacter();
    if (!primaryChar) return false;

    uintptr_t charAddr = reinterpret_cast<uintptr_t>(primaryChar);
    game::CharacterAccessor accessor(primaryChar);

    // ES: ── Vía 1: character+0x658 → activePlatoon → vtable+0x10 ── (CharacterHuman+0x658 = puntero a
    //     activePlatoon, el escuadrón activo del personaje).
    // ── Approach 1: character+0x658 → activePlatoon → vtable+0x10 ──
    // Research data: CharacterHuman+0x658 = activePlatoon pointer
    {
        uintptr_t activePlatoon = SEH_ReadPtr(charAddr + 0x658);
        if (activePlatoon > 0x10000 && activePlatoon < 0x00007FFFFFFFFFFF) {
            uintptr_t funcAddr = SEH_ReadVTableSlot(activePlatoon, 0x10);
            if (ValidateVTableCandidate(funcAddr, moduleBase, moduleSize, "char+0x658→AP→vt+0x10")) {
                funcs.SquadAddMember = reinterpret_cast<void*>(funcAddr);
                s_squadAddMemberDiscovered = true;
                spdlog::info("VTableDiscovery: SquadAddMember FOUND! char+0x658 → activePlatoon 0x{:X} → vtable+0x10 = 0x{:X} (RVA 0x{:X})",
                              activePlatoon, funcAddr, funcAddr - moduleBase);
                goto install_hook;
            }
        }
    }

    // ES: ── Vía 2: GetSquadPtr() → platoon → +0x1D8 → activePlatoon → vtable+0x10 ── (platoon+0x1D8 =
    //     activePlatoon*).
    // ── Approach 2: GetSquadPtr() → platoon → +0x1D8 → activePlatoon → vtable+0x10 ──
    // Research: platoon+0x1D8 = activePlatoon*
    {
        uintptr_t squadPtr = accessor.GetSquadPtr();
        if (squadPtr != 0) {
            uintptr_t activePlatoon = SEH_ReadPtr(squadPtr + 0x1D8);
            if (activePlatoon > 0x10000 && activePlatoon < 0x00007FFFFFFFFFFF) {
                uintptr_t funcAddr = SEH_ReadVTableSlot(activePlatoon, 0x10);
                if (ValidateVTableCandidate(funcAddr, moduleBase, moduleSize, "squad+0x1D8→AP→vt+0x10")) {
                    funcs.SquadAddMember = reinterpret_cast<void*>(funcAddr);
                    s_squadAddMemberDiscovered = true;
                    spdlog::info("VTableDiscovery: SquadAddMember FOUND! platoon 0x{:X} → +0x1D8 → activePlatoon 0x{:X} → vtable+0x10 = 0x{:X} (RVA 0x{:X})",
                                  squadPtr, activePlatoon, funcAddr, funcAddr - moduleBase);
                    goto install_hook;
                }
            }

            // ES: ── Vía 3: GetSquadPtr() → vtable+0x10 directamente ── por si GetSquadPtr devuelve ya un
            //     activePlatoon (no un platoon).
            // ── Approach 3: GetSquadPtr() → vtable+0x10 directly ──
            // In case GetSquadPtr returns an activePlatoon (not platoon)
            {
                uintptr_t funcAddr = SEH_ReadVTableSlot(squadPtr, 0x10);
                if (ValidateVTableCandidate(funcAddr, moduleBase, moduleSize, "squad→vt+0x10 direct")) {
                    funcs.SquadAddMember = reinterpret_cast<void*>(funcAddr);
                    s_squadAddMemberDiscovered = true;
                    spdlog::info("VTableDiscovery: SquadAddMember FOUND! squad 0x{:X} → vtable+0x10 = 0x{:X} (RVA 0x{:X})",
                                  squadPtr, funcAddr, funcAddr - moduleBase);
                    goto install_hook;
                }
            }
        }
    }

    // ES: Ninguna vía funcionó todavía — se reintentará en el siguiente tick.
    // No approach worked yet — will retry on next tick
    return false;

install_hook:
    // ES: Instalar el hook de escuadrón con la función descubierta. Si MinHook no puede, el puntero crudo
    //     sigue sirviendo a AddCharacterToLocalSquad.
    // Install the squad hook with the discovered function
    if (squad_hooks::Install()) {
        spdlog::info("VTableDiscovery: Squad hooks installed successfully");
        core.GetNativeHud().LogStep("OK", "SquadAddMember discovered via vtable + hook installed");
    } else {
        // Hook install failed (maybe vtable func can't be MinHooked),
        // but the raw function pointer is still usable by AddCharacterToLocalSquad
        spdlog::warn("VTableDiscovery: Squad hook install failed, but raw function pointer available");
        core.GetNativeHud().LogStep("WARN", "SquadAddMember found (vtable) — raw ptr OK, hook failed");
    }

    return true;
}

// ES: ── Máquina de estados de fase del cliente ──
// ── Client Phase State Machine ──

// ES: Cambia de fase de forma atómica y lo registra. Al entrar en Loading activa el guard de carga
//     de CharacterIterator (y lo quita al salir): así no se lee la "lektor" (lista de personajes del
//     motor) mientras se redimensiona, lo que corrompería el heap.
// EN: Atomically changes phase and logs it. Entering Loading enables the CharacterIterator loading
//     guard (and leaving it disables it): the engine "lektor" (character list) is not read while it
//     is being resized, which would corrupt the heap.
void Core::TransitionTo(ClientPhase newPhase) {
    ClientPhase old = m_clientPhase.exchange(newPhase, std::memory_order_release);
    if (old != newPhase) {
        spdlog::info("Core: Phase {} -> {}", ClientPhaseToString(old), ClientPhaseToString(newPhase));
        m_nativeHud.LogStep("PHASE", std::string(ClientPhaseToString(old)) + " -> " + ClientPhaseToString(newPhase));

        // Update loading state bridge for CharacterIterator safety guard.
        // CharacterIterator skips game memory reads during loading to prevent
        // heap corruption from non-atomic lektor reads.
        if (newPhase == ClientPhase::Loading) {
            game::SetGameLoadingState(true);
        } else if (old == ClientPhase::Loading) {
            game::SetGameLoadingState(false);
        }
    }
}

// ES: Lo llama HookPresent al detectar un hueco largo entre frames (carga bloqueante). Acepta desde
//     MainMenu, GameReady (Load dentro de partida) o Connected (flujo "conectar y luego cargar"), no
//     desde Startup. Rearma OnGameLoaded, limpia el estado stale si venía de una partida, re-arma los
//     fixes del host, aplica el parche de facción, activa el passthrough de carga de CharacterCreate
//     y pasa a Loading.
// EN: Called by HookPresent when it detects a long frame gap (blocking load). Accepts from MainMenu,
//     GameReady (in-game Load) or Connected ("connect then load" flow), not from Startup. Re-arms
//     OnGameLoaded, clears stale state if coming from a game, re-arms the host fixes, applies the
//     faction patch, enables CharacterCreate loading passthrough and moves to Loading.
void Core::OnLoadingGapDetected() {
    ClientPhase current = m_clientPhase.load(std::memory_order_acquire);

    // Accept from MainMenu (normal flow), GameReady (in-game Load button,
    // detected by render_hooks as >10s gap), o Connected (el jugador se conectó
    // DESDE EL MENÚ y AHORA carga la partida — flujo "connected-then-load").
    // NOT from Startup — engine initialization gaps are not save game loads.
    //
    // ⚠ FIX CRÍTICO (gameLoaded=false eterno): antes solo se aceptaba MainMenu/GameReady.
    //   Si el jugador conectaba al servidor ANTES de cargar su save (fase → Connected),
    //   el gap de carga se descartaba en render_hooks como "zone load" y PollForGameLoad
    //   retornaba de inmediato (fase==Connected). Resultado: OnGameLoaded NUNCA se
    //   disparaba → m_gameLoaded se quedaba en false para siempre → el Step 0 de
    //   OnGameTick (DIAG/combate/sim) jamás corría. Aceptando Connected aquí y
    //   transicionando a Loading, reutilizamos el mecanismo de detección existente
    //   (smooth-frame poll + timeouts) y, al completar, OnGameLoaded() ve m_connected==true
    //   y hace ResumeForNetwork + TransitionTo(GameReady) con normalidad.
    // EN: CRITICAL FIX (gameLoaded=false forever): previously only MainMenu/GameReady were accepted.
    //     If the player connected to the server BEFORE loading their save (phase → Connected), the load
    //     gap was discarded in render_hooks as a "zone load" and PollForGameLoad returned immediately
    //     (phase==Connected). Result: OnGameLoaded NEVER fired → m_gameLoaded stayed false forever →
    //     Step 0 of OnGameTick (DIAG/combat/sim) never ran. By accepting Connected here and moving to
    //     Loading, the existing detection mechanism is reused (smooth-frame poll + timeouts) and, when it
    //     completes, OnGameLoaded() sees m_connected==true and does ResumeForNetwork +
    //     TransitionTo(GameReady) as usual.
    if (current == ClientPhase::MainMenu || current == ClientPhase::GameReady ||
        current == ClientPhase::Connected) {
        // ES: Reiniciar el estado de partida cargada para que OnGameLoaded() pueda volver a dispararse con la
        //     nueva partida (si no, m_gameLoaded seguiría a true de la carga anterior).
        // Reset game-loaded state so OnGameLoaded() can fire again for the new save.
        // Without this, a second load would never trigger OnGameLoaded because
        // m_gameLoaded is already true from the first load.
        m_gameLoaded = false;
        m_initialEntityScanDone = false;
        m_spawnTeleportDone = false;
        m_needPollReset = true; // Reset PollForGameLoad statics on next call

        // ES: Si estábamos en GameReady, todos los punteros cacheados (locales Y remotos) ya son stale:
        //     limpiar todo para evitar UAF sobre objetos liberados. Además se re-arman todos los fixes
        //     one-shot del host (ver Reset* más abajo).
        // EN: All one-shot host fixes are also re-armed (see Reset* below).
        // If we were in GameReady, all cached game pointers (local AND remote)
        // are now stale. Clear everything to prevent UAF on freed game objects.
        if (current == ClientPhase::GameReady) {
            spdlog::info("Core: In-game save load — clearing ALL stale state");
            m_entityRegistry.Clear();
            m_interpolation.Clear();
            m_playerController.Reset();
            m_frameData[0].Clear();
            m_frameData[1].Clear();
            m_pipelineStarted = false;
            // Cambio 4: purgar cachés de personajes también en recarga de save en caliente.
            // Antes esto solo se hacía en el disconnect; sin ello, los punteros cacheados de la
            // partida anterior (own/other en shared_save_sync, tracker, remote-controlled) quedan
            // colgados sobre memoria que el motor libera al cargar la nueva save → UAF y el bug de
            // "no puedo pegar en frío". Se replica el mismo orden que el path de disconnect.
            // EN: Change 4: purge character caches on a hot save reload too. Before it was only done on
            //     disconnect; without it, cached pointers from the previous game (own/other in
            //     shared_save_sync, tracker, remote-controlled) dangle over memory the engine frees when loading
            //     the new save → UAF and the "cannot hit when cold" bug. Mirrors the disconnect path order.
            shared_save_sync::Reset();
            char_tracker_hooks::Clear();
            ai_hooks::ClearRemoteControlled();
            game::ResetProbeState();  // animClassOffset may differ between saves
            ResetHostFactionFix();    // re-armar el fix de facción del host para la nueva partida
            ResetNamelessResolve();   // purgar cachés de Nameless (Faction*/FR* pueden reciclarse)
            ResetHostFactionRelationsFix(); // re-armar el FIX-HOSTREL (relaciones corruptas) por si el motor recarga las relaciones al cargar el save
            ResetHostSimSeedFix();    // re-armar el seed de char+0xD0 (AI tick combate) para la nueva partida
            ResetHostMedSeedFix();    // re-armar el seed de char+0x4C0 (timestamp medico/comida) para la nueva partida
            ResetHostControlledCharFix(); // re-armar el FIX-CONTROL (SetControlledChar) para la nueva partida
            ResetHostCombatClassFix();    // re-armar el FIX-COMBATCLASS (CombatClass del host) para la nueva partida
            ResetHostCombatArmFix();      // re-armar el FIX-COMBATARM (arranque de la máquina de combate del host) para la nueva partida
            ResetHostArmHandFix();        // re-armar el FIX-ARMHAND (handle de brazo AI+0x318=Character+0x458) para la nueva partida
            ResetHostPlatoonFix();        // re-armar el FIX-PLATOON (re-enlace AI<->platoon) para la nueva partida
            ResetHostCombatAutotest();    // re-armar el [AUTOTEST] de combate para la nueva partida
            ResetHostSquadCloneDetect();  // re-armar el [DIAG-CLONESQUAD] (detección del clon del squad) para la nueva partida
            ResetHostSquadCloneDespawn(); // re-armar el [FIX-CLONESQUAD-DESPAWN] (despawn real del clon) para la nueva partida
        }

        // ES: Aplicar el parche de facción ANTES de empezar a cargar — determina qué facción controla el
        //     jugador al cargar la partida. Enfoque del mod de investigación: parchear el string de facción
        //     de 17 bytes en .rdata para que el juego nos asigne la facción correcta.
        // Apply faction patch BEFORE loading starts — this determines which
        // faction the player controls when the save loads.
        // Research mod approach: patch the 17-byte faction string in .rdata
        // so the game assigns us the correct player faction.
        if (m_lobbyManager.HasFaction()) {
            if (m_lobbyManager.ApplyFactionPatch()) {
                spdlog::info("Core: Faction string patched before save load");
                m_nativeHud.LogStep("LOBBY", "Faction patched: " + m_lobbyManager.GetFactionString());
            } else {
                spdlog::warn("Core: Faction patch FAILED — player may get wrong faction");
            }
        }

        // ES: Activar el passthrough de carga: el hook CharacterCreate sigue activo pero toma el camino
        //     ultraligero (timestamp + contador + llamada a la original). PollForGameLoad usa esos eventos
        //     para detectar el fin de la carga en vez de CharacterIterator (que corrompe el heap al cargar).
        // Enable loading passthrough: CharacterCreate hook stays active but takes
        // the ultra-lightweight path (timestamp + counter + call original).
        // PollForGameLoad uses these create events to detect loading completion
        // instead of CharacterIterator (which corrupts the heap during loading).
        entity_hooks::ResetLoadingCreateCount();
        entity_hooks::SetLoadingPassthrough(true);

        TransitionTo(ClientPhase::Loading);
    }
}

// ES: Cuenta personajes con CharacterIterator protegido con SEH (sin objetos con destructor en
//     __try). Devuelve -1 si la iteración peta.
// EN: Returns -1 if the iteration crashes.
// SEH-protected CharacterIterator count (no C++ objects with destructors in __try)
static int SEH_CharacterIteratorCount() {
    __try {
        game::CharacterIterator iter;
        return iter.Count();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_crashCount = 0;
        if (++s_crashCount <= 5) {
            OutputDebugStringA("KMP: SEH_CharacterIteratorCount crashed (SEH caught)\n");
        }
        return -1;
    }
}

// ES: Detecta el fin de la carga de partida (llamado desde el hilo de render y, como red de
//     seguridad, desde OnGameTick). Reintenta resolver PlayerBase/GameWorld, pone los puentes para
//     CharacterIterator y decide por eventos de CharacterCreate (no leyendo la lista de personajes,
//     que corrompería el heap). Tiene fallbacks anti-deadlock: CharacterIterator tras 30 sondeos,
//     "fallback definitivo" a los 60 y timeout duro incondicional a los 45 (pensado para Steam).
// EN: Detects the end of a game load (called from the render thread and, as a safety net, from
//     OnGameTick). Retries resolving PlayerBase/GameWorld, sets the CharacterIterator bridges and
//     decides from CharacterCreate events (not by reading the character list, which would corrupt
//     the heap). Has anti-deadlock fallbacks: CharacterIterator after 30 polls, "ultimate fallback"
//     at 60 and an unconditional hard timeout at 45 (meant for Steam).
void Core::PollForGameLoad() {
    if (m_gameLoaded) return;
    // ES: Sondear durante Loading (normal) y también en Startup/MainMenu como respaldo: la detección por
    //     hueco de carga puede perder cargas rápidas (el usuario pulsa Continue al instante).
    // Poll during Loading (normal), but also Startup/MainMenu as fallback.
    // Loading gap detection can miss fast loads (user clicks Continue immediately).
    ClientPhase phase = m_clientPhase.load(std::memory_order_acquire);
    // ⚠ FIX (gameLoaded=false eterno): antes retornábamos SIEMPRE en Connected, lo que
    //   bloqueaba la detección de carga en el flujo "connected-then-load" (conectar desde
    //   el menú y luego cargar el save). Ahora permitimos seguir si estamos Connected pero
    //   AÚN sin cargar (la red de seguridad de OnGameTick nos llama así). Connecting sí
    //   corta (el handshake todavía no terminó, no tiene sentido sondear la carga).
    // EN: FIX (gameLoaded=false forever): before, we ALWAYS returned while Connected, which blocked load
    //     detection in the "connected-then-load" flow (connect from the menu, then load the save). Now we
    //     keep going if Connected but NOT loaded yet (the OnGameTick safety net calls us like that).
    //     Connecting still returns (the handshake has not finished, polling the load makes no sense).
    if (phase == ClientPhase::Connecting) return;
    if (phase == ClientPhase::Connected && m_gameLoaded) return; // ya cargado: nada que hacer

    // ES: Intentar resolver los globales PlayerBase/GameWorld. Antes de cargar apuntan dentro del módulo
    //     o son 0; tras la carga contienen punteros de heap a objetos vivos del juego.
    // Try to resolve PlayerBase/GameWorld globals.
    // During initial scan (before game loads), these point into the module or are 0.
    // After the game loads, they contain heap pointers to live game objects.
    SEH_RetryGlobalDiscovery(m_scanner, m_gameFuncs);

    // ES: Validar: deben apuntar a objetos en el heap (fuera de la imagen del módulo).
    // Validate: must point to heap-allocated objects (outside module image)
    uintptr_t moduleBase = m_scanner.GetBase();
    size_t moduleSize = m_scanner.GetSize();
    auto validateGlobal = [moduleBase, moduleSize](uintptr_t addr) -> bool {
        if (addr == 0) return false;
        uintptr_t val = 0;
        if (!Memory::Read(addr, val)) return false;
        if (val < 0x10000 || val >= 0x00007FFFFFFFFFFF) return false;
        if (val >= moduleBase && val < moduleBase + moduleSize) return false;
        return true;
    };

    bool playerBaseValid = validateGlobal(m_gameFuncs.PlayerBase);
    // GameWorld 1.0.68 = instancia embebida: validateGlobal (espera *addr heap fuera del
    // modulo) la RECHAZARIA. Usar el validador especifico que acepta instancia-directa.
    // EN: GameWorld 1.0.68 = embedded instance: validateGlobal (expects *addr to be heap outside the
    //     module) would REJECT it. Use the specific validator that accepts a direct instance.
    bool gameWorldValid = ValidateGameWorldGlobal(m_gameFuncs.GameWorldSingleton, moduleBase, moduleSize);

    // ES: Reiniciar los estáticos en una segunda carga (flag puesto por OnLoadingGapDetected).
    // Reset statics on second load (flag set by OnLoadingGapDetected)
    static int s_pollCount = 0;
    static int s_noCharCount = 0;
    if (m_needPollReset) {
        m_needPollReset = false;
        s_pollCount = 0;
        s_noCharCount = 0;
        spdlog::info("Core::PollForGameLoad — reset statics for new load");
    }

    // ES: Poner los puentes para que CharacterIterator encuentre personajes cuando acabe la carga.
    // Set bridges so CharacterIterator can find characters AFTER loading is done
    if (playerBaseValid) game::SetResolvedPlayerBase(m_gameFuncs.PlayerBase);
    if (gameWorldValid) game::SetResolvedGameWorld(m_gameFuncs.GameWorldSingleton);

    // ES: ── Detección de carga por eventos de creación ── En lugar de CharacterIterator (que corrompe el
    //     heap al leer la lektor mientras el juego la redimensiona) se usan los eventos de entity_hooks:
    //     el hook CharacterCreate en modo passthrough solo actualiza un timestamp y un contador.
    //     Lógica: 1) loadingCreates > 0 → se están creando personajes (empezó la carga);
    //     2) timeSinceLastCreate > 3000 ms → los creates pararon (acabó la ráfaga);
    //     3) ambas → mundo cargado, se puede seguir.
    // ── Create-event-based loading detection ──
    // Instead of CharacterIterator (which corrupts the heap by reading the lektor
    // while the game is actively resizing it), use entity_hooks create events.
    // The CharacterCreate hook fires in loading passthrough mode during loading,
    // updating a timestamp and counter without touching game memory structures.
    //
    // Detection logic:
    //   1. loadingCreates > 0  →  characters ARE being created (loading started)
    //   2. timeSinceLastCreate > 3000ms  →  creates STOPPED (loading burst finished)
    //   3. BOTH conditions true  →  game world is fully loaded, safe to proceed
    int loadingCreates = entity_hooks::GetLoadingCreateCount();
    int64_t timeSinceCreate = entity_hooks::GetTimeSinceLastCreate();

    s_pollCount++;

    if (loadingCreates > 0 && timeSinceCreate > 3000) {
        // ES: Hubo creates y pararon — terminó la ráfaga de carga: quitar el passthrough y lanzar OnGameLoaded.
        // Characters were created and they stopped — loading burst finished
        spdlog::info("Core::PollForGameLoad — {} creates detected, last create {}ms ago. "
                     "Loading burst finished! Triggering OnGameLoaded",
                     loadingCreates, timeSinceCreate);
        m_nativeHud.LogStep("GAME", "Loading complete (" + std::to_string(loadingCreates) +
                            " creates, quiet for " + std::to_string(timeSinceCreate) + "ms)");

        // Disable loading passthrough before OnGameLoaded enables full hook
        entity_hooks::SetLoadingPassthrough(false);

        OnGameLoaded();
    } else if (loadingCreates > 0) {
        // ES: Hay creates en curso — la carga sigue.
        // Creates are happening — loading is in progress
        if (s_pollCount <= 5 || s_pollCount % 10 == 0) {
            spdlog::debug("Core::PollForGameLoad — {} creates so far, last {}ms ago (loading in progress, poll #{})",
                         loadingCreates, timeSinceCreate, s_pollCount);
        }
    } else {
        // ES: Aún no hay creates — esperando a que empiece la carga.
        // No creates yet — waiting for loading to start
        s_noCharCount++;
        if (s_noCharCount <= 5 || s_noCharCount % 10 == 0) {
            spdlog::debug("Core::PollForGameLoad — no creates yet, globals valid={}/{} (poll #{})",
                         playerBaseValid, gameWorldValid, s_noCharCount);
        }

        // ES: Respaldo: si los globales son válidos pero no llegan creates, puede que el hook CharacterCreate
        //     no esté disparando (falló la instalación o la carga no crea personajes). Se prueba
        //     CharacterIterator como último recurso (la lektor ya debería ser estable). OJO: los comentarios
        //     hablan de segundos (60 s / 120 s) pero el umbral real es de sondeos (30 / 60); el tiempo
        //     depende de la cadencia de llamada.
        // EN: NOTE: the comments talk about seconds (60 s / 120 s) but the real threshold is in polls
        //     (30 / 60); the actual time depends on how often this is called.
        // Fallback: if globals are valid for 60+ seconds but no creates detected,
        // the CharacterCreate hook may not be firing (hook install failed, or game
        // loaded via a path that doesn't create characters). Try CharacterIterator
        // as a last resort — by now loading should be complete so the lektor is stable.
        if (s_noCharCount >= 30 && (playerBaseValid || gameWorldValid)) {
            int charCount = SEH_CharacterIteratorCount();
            if (charCount > 0) {
                spdlog::warn("Core::PollForGameLoad — fallback: CharacterIterator found {} chars "
                             "after {} polls with no create events", charCount, s_noCharCount);
                m_nativeHud.LogStep("GAME", "Game loaded (CharacterIterator fallback, " +
                                    std::to_string(charCount) + " chars)");
                entity_hooks::SetLoadingPassthrough(false);
                OnGameLoaded();
            } else if (s_noCharCount >= 60) {
                spdlog::warn("Core::PollForGameLoad — ultimate fallback: 120s with valid globals, "
                             "no creates, no chars. Assuming loaded.");
                m_nativeHud.LogStep("GAME", "Game assumed loaded (ultimate fallback after 120s)");
                entity_hooks::SetLoadingPassthrough(false);
                OnGameLoaded();
            }
        }

        // ES: ARREGLO CRÍTICO: timeout duro incondicional para el deadlock de Steam. Si GameWorldSingleton no
        //     se resuelve (bug de Steam), los globales nunca son válidos y el timeout anterior no salta nunca;
        //     este fuerza OnGameLoaded a los 45 sondeos ("90 s") pase lo que pase.
        // CRITICAL FIX: Unconditional hard timeout for Steam deadlock case
        // If GameWorldSingleton unresolved (Steam bug), globals never become valid,
        // so the above 120s timeout never fires. Add hard 90s timeout regardless.
        if (s_noCharCount >= 45) {
            spdlog::error("Core::PollForGameLoad — HARD TIMEOUT: 90s with no loading detection! "
                         "Forcing OnGameLoaded (Steam deadlock prevention)");
            m_nativeHud.AddSystemMessage("WARNING: Force-loaded after 90s timeout!");
            m_nativeHud.LogStep("WARN", "Hard timeout (90s) - forced GameLoaded");
            entity_hooks::SetLoadingPassthrough(false);
            OnGameLoaded();
        }
    }
}

// ES: Declaraciones adelantadas de los helpers SEH usados en el enlace diferido de personajes del mod.
// Forward declarations for SEH helpers used in OnGameLoaded's deferred mod link
static bool SEH_FallbackPostSpawnSetup(void* character, EntityID netId, PlayerID owner, Vec3 pos);
static bool SEH_AllyModFaction(void* character);

// ES: "El mundo está listo". Se ejecuta una sola vez por carga (exchange de m_gameLoaded). Pasa a
//     GameReady, quita los guards de carga, reanuda la red si ya estaba conectado, instala hooks
//     post-carga (movimiento, escuadrón, recursos), re-descubre PlayerBase/GameWorld, notifica a
//     PlayerController, captura el factory de personajes, prepara el sistema de spawn, enlaza
//     personajes del mod pendientes, auto-conecta si procede, envía PlayerReady y vuelca al log
//     todas las funciones y offsets resueltos.
// EN: "The world is ready". Runs once per load (exchange on m_gameLoaded). Moves to GameReady,
//     clears loading guards, resumes networking if already connected, installs post-load hooks
//     (movement, squad, resources), re-discovers PlayerBase/GameWorld, notifies PlayerController,
//     captures the character factory, prepares the spawn system, links pending mod characters,
//     auto-connects if configured, sends PlayerReady and dumps every resolved function and offset.
void Core::OnGameLoaded() {
    if (m_gameLoaded.exchange(true)) return; // Only run once

    // Transition to GameReady phase
    TransitionTo(ClientPhase::GameReady);

    m_nativeHud.LogStep("GAME", "=== Game world loaded! ===");
    OutputDebugStringA("KMP: === Core::OnGameLoaded() START ===\n");

    // ES: ── Quitar el guard de carga de los hooks ──
    // ── Clear loading guard ──
    building_hooks::SetLoading(false);
    inventory_hooks::SetLoading(false);
    squad_hooks::SetLoading(false);
    faction_hooks::SetLoading(false);
    m_loadingOrch.OnGameLoaded();
    m_nativeHud.LogStep("GAME", "Loading guard cleared (hooks active)");

    // ES: ═══ Reanudación de red diferida ═══ Si el jugador conectó desde el menú (antes de cargar),
    //     ResumeForNetwork() se aplazó para no activar CharacterCreate durante los 130+ creates de la
    //     carga. Ya acabó la carga: se activa ahora junto con los hooks de combate y diagnóstico.
    // ═══ Deferred network resume ═══
    // If the player connected from the main menu (before game loaded),
    // ResumeForNetwork() was deferred to avoid enabling CharacterCreate
    // during the 130+ loading creates. Now that loading is done, enable it.
    if (m_connected.load()) {
        entity_hooks::ResumeForNetwork();
        HookManager::Get().Enable("CharacterDeath");
        HookManager::Get().Enable("CharacterKO");
        // [DIAG-PUSHORDER] El slot "StartAttack" (0x722EF0 UI/MyGUI) quedó refutado y NO se
        // instala. El DIAG de encolado real es ahora Tasker::pushOrder 0x674300. Lo activamos
        // aquí también (ruta "conectado antes de cargar") para confirmar si la orden de ataque
        // del host entra en su Tasker.
        // EN: [DIAG-PUSHORDER] The "StartAttack" slot (0x722EF0 UI/MyGUI) was refuted and is NOT installed.
        //     The real enqueue DIAG is now Tasker::pushOrder 0x674300. It is also enabled here ("connected
        //     before load" path) to confirm whether the host attack order enters its Tasker.
        HookManager::Get().Enable("PushOrder");
        // [DIAG-COMBATSEED] Activa también aquí (ruta "conectado antes de cargar") el hook de
        // CombatClass::update 0x60D650 para capturar el estado de combate del CombatClass del host.
        // EN: [DIAG-COMBATSEED] Also enabled here ("connected before load" path): the CombatClass::update
        //     0x60D650 hook, to capture the combat state of the host CombatClass.
        HookManager::Get().Enable("CombatClassUpdate");
        spdlog::info("Core: Deferred ResumeForNetwork — entity hooks enabled post-load (incl. PushOrder + CombatSeed DIAG)");
        m_nativeHud.LogStep("NET", "Sync starting (connected before load)");
        m_nativeHud.AddSystemMessage("Game loaded — syncing with server...");
    }

    // ES: ═══ Instalar los hooks que se aplazaron hasta después de cargar ═══ Combate, inventario,
    //     facción, tiempo e IA ya se instalaron en InitHooks; solo quedan movimiento, escuadrón y recursos.
    // ═══ Install remaining hooks that were deferred to post-load ═══
    // Combat, inventory, faction, time, and AI hooks are already installed
    // in InitHooks. Only movement, squad, and resource hooks remain deferred.
    m_nativeHud.LogStep("HOOK", "Installing post-load hooks...");
    OutputDebugStringA("KMP: OnGameLoaded — installing post-load hooks\n");

    // ES: Hooks de movimiento (CharacterMoveTo — NULL en Steam; ahí se usa sondeo de posición).
    // Movement hooks (CharacterMoveTo — null on Steam, position polling works instead)
    if (m_gameFuncs.CharacterMoveTo) {
        if (movement_hooks::Install()) {
            m_nativeHud.LogStep("OK", "Movement hooks installed");
        } else {
            m_nativeHud.LogStep("WARN", "Movement hooks FAILED");
        }
    } else {
        m_nativeHud.LogStep("INFO", "Movement hooks skipped (position polling active)");
    }

    // ES: Hooks de escuadrón.
    // Squad hooks
    if (m_gameFuncs.SquadAddMember) {
        if (squad_hooks::Install()) {
            m_nativeHud.LogStep("OK", "Squad hooks installed");
        } else {
            m_nativeHud.LogStep("WARN", "Squad hooks FAILED");
        }
    }

    // ES: Hooks de recursos (descubrimiento de la vtable de Ogre).
    // Resource hooks (Ogre VTable discovery)
    if (resource_hooks::Install()) {
        m_nativeHud.LogStep("OK", "Resource hooks installed");
    } else {
        m_nativeHud.LogStep("INFO", "Resource hooks deferred (burst-detection fallback)");
    }
    // ES: Descubrimiento diferido de globales (PlayerBase y GameWorld). En el escaneo inicial suelen
    //     valer 0 porque Kenshi aún no los ha inicializado; con la partida cargada ya se pueden validar.
    // Run deferred global discovery — both PlayerBase and GameWorld.
    // During initial scan (before game loads), these globals are typically 0
    // because Kenshi hasn't initialized them yet. Now that the game is loaded,
    // we can find and validate them.
    m_nativeHud.LogStep("SCAN", "PlayerBase/GameWorld discovery...");

    // ES: Validar que un global contenga un objeto en el HEAP (no un puntero interno del módulo).
    //     Bug anterior: direcciones de .data con punteros a .text pasaban la validación y
    //     CharacterIterator leía basura y encontraba 0 personajes.
    // Validate that a global pointer contains a HEAP-allocated object (not a module-internal pointer).
    // Previous bug: .data addresses containing .text pointers passed validation,
    // causing CharacterIterator to read garbage and find 0 characters.
    uintptr_t moduleBase = m_scanner.GetBase();
    size_t moduleSize = m_scanner.GetSize();
    auto validateGlobal = [moduleBase, moduleSize](uintptr_t addr) -> bool {
        if (addr == 0) return false;
        uintptr_t val = 0;
        if (!Memory::Read(addr, val)) return false;
        if (val < 0x10000 || val >= 0x00007FFFFFFFFFFF) return false;
        // MUST be outside module image — real game objects are heap-allocated
        if (val >= moduleBase && val < moduleBase + moduleSize) return false;
        return true;
    };

    // GameWorld 1.0.68 = instancia embebida -> validador especifico (no validateGlobal).
    // EN: GameWorld 1.0.68 = embedded instance -> specific validator (not validateGlobal).
    bool needsRetry = !validateGlobal(m_gameFuncs.PlayerBase) ||
                      !ValidateGameWorldGlobal(m_gameFuncs.GameWorldSingleton, moduleBase, moduleSize);

    if (needsRetry) {
        m_nativeHud.LogStep("SCAN", "Retrying global discovery (game now loaded)...");
        SEH_RetryGlobalDiscovery(m_scanner, m_gameFuncs);

        // ES: Re-validar tras el reintento — puede volver a resolver la misma basura. Para GameWorld solo se
        //     limpia si hay algo que no es GameWorld (se conserva el NULL temprano mientras carga).
        // EN: For GameWorld it is only cleared if something non-GameWorld is there (early NULL is kept).
        // Re-validate after retry — SEH_RetryGlobalDiscovery may re-resolve to same garbage
        if (m_gameFuncs.PlayerBase != 0 && !validateGlobal(m_gameFuncs.PlayerBase)) {
            spdlog::warn("Core: PlayerBase still garbage after retry — clearing");
            m_gameFuncs.PlayerBase = 0;
        }
        if (m_gameFuncs.GameWorldSingleton != 0 &&
            !ValidateGameWorldGlobal(m_gameFuncs.GameWorldSingleton, moduleBase, moduleSize)) {
            // Solo limpiar si hay algo no-GameWorld; preservar NULL temprano (aun cargando).
            uintptr_t fw = 0;
            Memory::Read(m_gameFuncs.GameWorldSingleton, fw);
            if (fw != 0) {
                spdlog::warn("Core: GameWorldSingleton still not a GameWorld instance after retry — clearing");
                m_gameFuncs.GameWorldSingleton = 0;
            }
        }

        if (m_gameFuncs.PlayerBase != 0) {
            game::SetResolvedPlayerBase(m_gameFuncs.PlayerBase);
            m_nativeHud.LogStep("OK", "PlayerBase discovered");
        } else {
            m_nativeHud.LogStep("WARN", "PlayerBase NOT found — faction capture will use entity_hooks bootstrap");
        }
        if (m_gameFuncs.GameWorldSingleton != 0) {
            m_nativeHud.LogStep("OK", "GameWorld discovered");
        } else {
            m_nativeHud.LogStep("WARN", "GameWorld NOT found — CharacterIterator may fail");
        }
    } else {
        m_nativeHud.LogStep("OK", "PlayerBase + GameWorld already valid");
    }

    // ES: Poner siempre el puente de GameWorld (CharacterIterator lo usa de respaldo si falla PlayerBase).
    // Always set GameWorld bridge (CharacterIterator uses it as fallback when PlayerBase fails)
    if (m_gameFuncs.GameWorldSingleton != 0) {
        game::SetResolvedGameWorld(m_gameFuncs.GameWorldSingleton);
        m_nativeHud.LogStep("OK", "GameWorld bridge set (fallback for CharacterIterator)");
    } else if (m_isSteamVersion) {
        // ── FIX connected-then-load (entities=0): bridge de respaldo a la instancia embebida ──
        // Si el Scanner dejó GameWorldSingleton=0 (p.ej. la cadena player/faction aún no
        // resolvía cuando corrió RetryGlobalDiscovery en este flujo), forzamos el hardcoded
        // base+0x2134110. En Steam 1.0.68 GameWorld es una INSTANCIA estática embebida en esa
        // RVA (confirmado por RE: primer qword = vtable .text 0x1722608). Solo seteamos el
        // bridge si valida estructuralmente (vtable en .text) para no propagar basura. Sin esto,
        // CharacterIterator::Reset Strategy 2 (GW+0x580 -> +0x2B0) NUNCA corre y entities=0.
        // EN: ── FIX connected-then-load (entities=0): fallback bridge to the embedded instance ──
        //     If the Scanner left GameWorldSingleton=0 (e.g. the player/faction chain did not resolve yet
        //     when RetryGlobalDiscovery ran in this flow), force the hard-coded base+0x2134110. In Steam
        //     1.0.68 GameWorld is a static INSTANCE embedded at that RVA (confirmed by RE: first qword =
        //     .text vtable 0x1722608). The bridge is only set if it validates structurally (vtable in .text)
        //     so no garbage is propagated. Without this, CharacterIterator::Reset Strategy 2
        //     (GW+0x580 -> +0x2B0) NEVER runs and entities=0.
        uintptr_t embeddedGw = moduleBase + 0x2134110;
        if (ValidateGameWorldGlobal(embeddedGw, moduleBase, moduleSize)) {
            m_gameFuncs.GameWorldSingleton = embeddedGw;        // re-hidratar el campo para el resto del flujo
            game::SetResolvedGameWorld(embeddedGw);
            spdlog::warn("Core: GameWorldSingleton recuperado vía instancia embebida hardcoded "
                         "0x{:X} (Scanner lo dejó en 0) — bridge seteado", embeddedGw);
            m_nativeHud.LogStep("OK", "GameWorld bridge set (instancia embebida hardcoded)");
        } else {
            m_nativeHud.LogStep("WARN", "GameWorld instancia embebida aún no válida — bridge diferido al tick");
        }
    }

    // ES: Ahora avisar a PlayerController (puede usar CharacterIterator si PlayerBase está resuelto).
    // Now notify player controller (can use CharacterIterator if PlayerBase is resolved)
    m_nativeHud.LogStep("GAME", "Player controller: OnGameWorldLoaded...");
    SEH_PlayerControllerOnGameWorldLoaded(m_playerController);
    m_nativeHud.LogStep("OK", "Player controller ready");

    // ES: ═══ Descubrimiento diferido de SquadAddMember por vtable ═══ Si fallaron el escaneo de patrones
    //     y el fallback por strings, se busca en la vtable de un escuadrón vivo (vtable+0x10 = añadir
    //     personaje al escuadrón, según la tabla CT).
    // ═══ Deferred vtable discovery for SquadAddMember ═══
    // If pattern scan + string fallback failed, try to discover from a live squad vtable.
    // CT data: "Squad vtable+0x10: adds character to squad"
    if (!m_gameFuncs.SquadAddMember) {
        m_nativeHud.LogStep("SCAN", "SquadAddMember: vtable discovery...");
        if (TryDiscoverSquadAddMemberFromVTable(m_gameFuncs, moduleBase, moduleSize)) {
            m_nativeHud.LogStep("OK", "SquadAddMember found via vtable!");
        } else {
            m_nativeHud.LogStep("INFO", "SquadAddMember: vtable discovery deferred (no squad yet)");
        }
    }

    // ═══ FIX-FACTORY-GW #1: capturar theFactory desde GameWorld+0x5B0 ═══
    // CAUSA RAÍZ del spawn de remotos roto: el hook CharacterCreate (única fuente previa del
    // m_factory) NO dispara al conectar con la partida YA cargada (no hay creates nuevos), así
    // que IsReady()=false y el gate del heap scan + el de spawn bloqueaban todo el flujo.
    // Aquí capturamos el factory directamente de la instancia GameWorld (GW+0x5B0, RVA absoluto
    // 0x21345B0; confirmado en docs/reverse-engineering/kenshi-re-memory.md línea 47), sin
    // depender del hook -> IsReady() pasa a true. Tras flag kCaptureFactoryFromGameWorld (=true).
    // EN: ═══ FIX-FACTORY-GW #1: capture theFactory from the GameWorld instance ═══
    //     ROOT CAUSE of broken remote spawning: the CharacterCreate hook (the only previous source of
    //     m_factory) does NOT fire when connecting to an ALREADY loaded game (no new creates), so
    //     IsReady()=false and both the heap-scan gate and the spawn gate blocked the whole flow. Here the
    //     factory is captured straight from the GameWorld instance, without depending on the hook ->
    //     IsReady() becomes true. Behind flag kCaptureFactoryFromGameWorld (=true).
    //     WARNING (inconsistency): the header above says GW+0x5B0, but the NOTE below (2026-06-20) and
    //     the math (0x2134110 + 0x4A0 = 0x21345B0) show the real field is GW+0x4A0, i.e. the global
    //     theFactory at RVA 0x21345B0. The "+0x5B0" in the header is outdated.
    // ES: AVISO (incoherencia): la cabecera de arriba dice GW+0x5B0, pero la NOTA de abajo (2026-06-20) y
    //     la cuenta (0x2134110 + 0x4A0 = 0x21345B0) indican que el campo real es GW+0x4A0, es decir, el
    //     global theFactory en la RVA 0x21345B0. El "+0x5B0" de la cabecera está desfasado.
    if (m_gameFuncs.GameWorldSingleton != 0 && !m_spawnManager.IsReady()) {
        // NOTA (2026-06-20): el factory es el puntero global theFactory @ RVA 0x21345B0
        // (== GW+0x4A0, NO +0x5B0). Si aquí aún está NULL (el host acaba de empezar NEW GAME
        // y todavía no creó su 1er personaje), el reintento por tick FIX-FACTORY-RETRY lo
        // capturará en cuanto el juego llene el slot. Ver core.cpp OnGameTick.
        // EN: NOTE (2026-06-20): the factory is the global pointer theFactory @ RVA 0x21345B0
        //     (== GW+0x4A0, NOT +0x5B0). If it is still NULL here (the host just started a NEW GAME and has
        //     not created their first character yet), the per-tick retry FIX-FACTORY-RETRY captures it as
        //     soon as the game fills the slot. See OnGameTick in core.cpp.
        m_nativeHud.LogStep("GAME", "Capturando factory (theFactory @0x21345B0)...");
        if (m_spawnManager.CaptureFactoryFromGameWorld(m_gameFuncs.GameWorldSingleton)) {
            m_nativeHud.LogStep("OK", "Factory capturado @0x21345B0 (IsReady=true)");
        } else {
            m_nativeHud.LogStep("WARN", "Factory @0x21345B0 aún NULL — reintento por tick activo");
        }
    }

    // ES: ═══ Comprobar que el sistema de spawn está listo ═══
    // ═══ Verify spawn system readiness ═══
    m_nativeHud.LogStep("GAME", "Verifying spawn system...");
    bool spawnReady = m_spawnManager.VerifyReadiness();
    if (spawnReady) {
        m_nativeHud.LogStep("OK", "Spawn system ready");
    } else {
        m_nativeHud.LogStep("WARN", "Spawn system NOT ready — remote characters may fail");
    }

    // ES: Escaneo temprano del heap buscando plantillas (GameData de personajes) — se intenta incluso sin
    //     factory, siempre que haya ALGÚN dato del que partir; luego se buscan las plantillas del mod.
    // Early heap scan — try even without factory, as long as we have ANY data to bootstrap from
    if (m_spawnManager.GetManagerPointer() != 0 || m_spawnManager.GetTemplateCount() > 0) {
        m_nativeHud.LogStep("GAME", "Early heap scan for templates...");
        SEH_ScanGameDataHeap(m_spawnManager);
        m_nativeHud.LogStep("OK", "Heap scan done (" + std::to_string(m_spawnManager.GetTemplateCount()) + " templates)");
        m_spawnManager.FindModTemplates();
        if (m_spawnManager.GetModTemplateCount() > 0) {
            m_nativeHud.LogStep("MOD", "Found " + std::to_string(m_spawnManager.GetModTemplateCount()) + " mod player templates");
        }
    } else if (m_spawnManager.IsReady()) {
        // ES: Factory capturado pero sin puntero al manager — se escanea igualmente.
        // Factory captured but no manager pointer yet — still try scan
        m_nativeHud.LogStep("GAME", "Heap scan (factory only, no manager ptr)...");
        SEH_ScanGameDataHeap(m_spawnManager);
        m_nativeHud.LogStep("OK", "Heap scan done (" + std::to_string(m_spawnManager.GetTemplateCount()) + " templates)");
        m_spawnManager.FindModTemplates();
        if (m_spawnManager.GetModTemplateCount() > 0) {
            m_nativeHud.LogStep("MOD", "Found " + std::to_string(m_spawnManager.GetModTemplateCount()) + " mod player templates");
        }
    }

    // ES: Quitar el passthrough de carga — CharacterCreate vuelve a ejecutar el cuerpo completo. La carga
    //     terminó, así que los spawns de NPC en tiempo de juego (de uno en uno) pasan por el hook completo
    //     para registro de entidad, captura de facción y secuestro de NPC.
    // Disable loading passthrough — CharacterCreate hook now runs full body.
    // Loading is complete, so runtime NPC spawns (single/few at a time) go through
    // the full hook for entity registration, faction capture, and NPC hijack.
    entity_hooks::SetLoadingPassthrough(false);

    // ES: Registrar las plantillas de personajes del mod capturadas durante el passthrough de carga.
    // Log mod template characters captured during loading passthrough
    {
        void* modTemplates[16] = {};
        int modCount = entity_hooks::GetCapturedModTemplates(modTemplates, 16);
        if (modCount > 0) {
            spdlog::info("Core::OnGameLoaded — {} mod template characters captured during loading", modCount);
            m_nativeHud.LogStep("MOD", "Captured " + std::to_string(modCount) + " mod templates during load");
        }
    }

    // ES: Asegurar que el hook CharacterCreate está activado (debería estarlo desde la instalación, pero
    //     se reactiva por si la ruta de captura durante la carga lo desactivó).
    // Ensure CharacterCreate hook is enabled (it should already be from install,
    // but re-enable in case it was disabled by the loading capture code path).
    if (HookManager::Get().Enable("CharacterCreate")) {
        spdlog::info("Core::OnGameLoaded — CharacterCreate hook ENABLED (full mode for runtime spawns)");
        m_nativeHud.LogStep("HOOK", "CharacterCreate enabled (post-load)");
    } else {
        spdlog::warn("Core::OnGameLoaded — CharacterCreate Enable() returned false");
        m_nativeHud.LogStep("WARN", "CharacterCreate enable failed");
    }

    // ES: ═══ Enlace diferido de personajes del mod (ruta "conectar antes de cargar") ═══ Si se conectó
    //     antes de cargar, HandleEntitySpawn no pudo enlazar los personajes del mod (CharacterIterator
    //     necesita la partida cargada). Ahora se enlaza cada entidad remota sin objeto a su "Player N",
    //     se marca como controlada remotamente, se hace el setup post-spawn, se escribe el nombre de
    //     GameData y se alía su facción.
    // ═══ Deferred mod character link (connect-before-load path) ═══
    // If we connected before the game loaded, HandleEntitySpawn couldn't link mod characters
    // (CharacterIterator needs game loaded). Now that the game is loaded, try to link any
    // remote entities that are still missing game objects to their mod characters.
    if (m_connected.load()) {
        auto remoteEntities = m_entityRegistry.GetRemoteEntities();
        int deferredLinks = 0;
        for (EntityID eid : remoteEntities) {
            if (m_entityRegistry.GetGameObject(eid) != nullptr) continue; // Already linked
            auto info = m_entityRegistry.GetInfo(eid);
            if (!info) continue;

            void* modChar = FindModCharacterBySlot(static_cast<int>(info->ownerPlayerId));
            if (modChar && m_entityRegistry.GetNetId(modChar) == INVALID_ENTITY) {
                m_entityRegistry.SetGameObject(eid, modChar);
                m_entityRegistry.UpdatePosition(eid, info->lastPosition);
                ai_hooks::MarkRemoteControlled(modChar);
                SEH_FallbackPostSpawnSetup(modChar, eid, info->ownerPlayerId, info->lastPosition);
                // ES: Es seguro escribir el nombre de la plantilla GameData — cada slot del mod tiene plantilla propia.
                // Safe to write GameData template name — mod characters have unique per-slot templates
                m_playerController.WriteGameDataNameForModLink(modChar, info->ownerPlayerId);
                SEH_AllyModFaction(modChar);
                deferredLinks++;
                spdlog::info("Core: OnGameLoaded deferred link: entity {} -> mod char for player {}",
                             eid, info->ownerPlayerId);
            }
        }
        if (deferredLinks > 0) {
            m_nativeHud.AddSystemMessage("Linked " + std::to_string(deferredLinks) + " remote player(s) (deferred)");
            m_nativeHud.LogStep("OK", std::to_string(deferredLinks) + " deferred mod links");
        }
    }

    // ES: ═══ AUTO-CONEXIÓN AL CARGAR ═══ CRÍTICO: conectar DESPUÉS de cargar, nunca en el menú principal,
    //     para que el mundo esté listo antes de cualquier sincronización de red.
    // ═══ AUTO-CONNECT ON GAME LOAD ═══
    // CRITICAL: Connect AFTER game loads, not in main menu!
    // This ensures world is ready before any network sync happens
    if (!m_connected.load() && m_config.autoConnect) {
        spdlog::info("Core::OnGameLoaded — Auto-connecting to {}:{}",
                     m_config.lastServer, m_config.lastPort);
        m_nativeHud.AddSystemMessage("Game loaded - connecting to server...");

        // ES: ConnectAsync solo recibe 2 argumentos (dirección, puerto).
        // ConnectAsync takes only 2 arguments (address, port)
        if (m_client.ConnectAsync(m_config.lastServer, m_config.lastPort)) {
            m_clientPhase.store(ClientPhase::Connecting);
            // FIX #3 (Zero): no spamear el HUD con "Connecting to 162.248.94.149:27800"
            // (la IP del master server). Solo mostramos el mensaje en el HUD para
            // conexiones locales/privadas (127.0.0.1, 10.x, 192.168.x, 172.16-31.x).
            // Para IPs públicas (master server) lo dejamos SOLO en el log de archivo.
            // NOTA: NO se toca la conexión en sí (ConnectAsync ya se ejecutó arriba),
            // solo se silencia el mensaje del HUD. La conexión local sigue intacta.
            // EN: FIX #3: do not spam the HUD with "Connecting to 162.248.94.149:27800" (the master server IP).
            //     The HUD message is only shown for local/private connections (127.0.0.1, 10.x, 192.168.x,
            //     172.16-31.x); for public IPs (master server) it goes ONLY to the log file. The connection
            //     itself is NOT touched (ConnectAsync already ran above), only the HUD message is silenced.
            //     Note: the "172." check matches every 172.x address, not only 172.16-31.
            // ES: Nota: la comprobación "172." acepta cualquier 172.x, no solo 172.16-31.
            const std::string& srv = m_config.lastServer;
            bool esLocal = (srv.rfind("127.", 0) == 0) ||   // loopback
                           (srv.rfind("10.", 0) == 0) ||    // privada clase A
                           (srv.rfind("192.168.", 0) == 0) || // privada clase C
                           (srv.rfind("172.", 0) == 0) ||   // privada clase B (172.16-31)
                           (srv == "localhost");
            std::string msgConn = "Connecting to " + srv + ":" + std::to_string(m_config.lastPort);
            if (esLocal) {
                m_nativeHud.LogStep("NET", msgConn + "..."); // HUD + log para local
            } else {
                spdlog::info("Core: {} (master/remoto — silenciado en HUD)", msgConn); // solo archivo
            }
        } else {
            m_nativeHud.AddSystemMessage("Connection failed - check server");
        }
    }

    // ES: ═══ HANDSHAKE DE "LISTO" MULTIJUGADOR ═══ CRÍTICO: avisar al servidor de que estamos listos
    //     para spawnear; el servidor espera a que TODOS los jugadores envíen PlayerReady.
    // ═══ MULTIPLAYER READY HANDSHAKE ═══
    // CRITICAL: Tell server we're ready to spawn
    // Server will wait for ALL players to send PlayerReady before allowing spawns
    if (m_connected.load()) {
        spdlog::info("Core::OnGameLoaded — Sending C2S_PlayerReady to server");
        m_nativeHud.AddSystemMessage("Player ready - waiting for others...");

        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_PlayerReady);
        m_client.SendReliable(writer.Data(), writer.Size()); // Use public SendReliable()

        m_nativeHud.LogStep("NET", "Sent PlayerReady - waiting for all players");
    }

    // ES: ═══ VOLCADO DE TODAS LAS FUNCIONES Y OFFSETS ═══ Referencia de ingeniería inversa en tiempo de
    //     ejecución: dirección absoluta y RVA (dirección - base del módulo) de cada función resuelta.
    // EN: ═══ DUMP OF ALL FUNCTIONS AND OFFSETS ═══ Runtime reverse-engineering reference: absolute
    //     address and RVA (address - module base) of every resolved function.
    // ═══ DUMP ALL FUNCTIONS AND OFFSETS ═══
    {
        uintptr_t base = m_scanner.GetBase();
        auto fmtPtr = [base](const char* name, void* ptr) {
            if (ptr)
                spdlog::info("  FUNC  {:24s} = 0x{:X}  (RVA 0x{:X})", name,
                             reinterpret_cast<uintptr_t>(ptr),
                             reinterpret_cast<uintptr_t>(ptr) - base);
            else
                spdlog::info("  FUNC  {:24s} = NULL", name);
        };
        spdlog::info("=== FUNCTION DUMP (base=0x{:X}) ===", base);
        fmtPtr("CharacterSpawn",       m_gameFuncs.CharacterSpawn);
        fmtPtr("CharacterDestroy",     m_gameFuncs.CharacterDestroy);
        fmtPtr("CreateRandomSquad",    m_gameFuncs.CreateRandomSquad);
        fmtPtr("CharacterSerialise",   m_gameFuncs.CharacterSerialise);
        fmtPtr("CharacterKO",          m_gameFuncs.CharacterKO);
        fmtPtr("CharacterSetPosition", m_gameFuncs.CharacterSetPosition);
        fmtPtr("CharacterMoveTo",      m_gameFuncs.CharacterMoveTo);
        fmtPtr("ApplyDamage",          m_gameFuncs.ApplyDamage);
        fmtPtr("StartAttack",          m_gameFuncs.StartAttack);
        fmtPtr("CharacterDeath",       m_gameFuncs.CharacterDeath);
        fmtPtr("HealthUpdate",         m_gameFuncs.HealthUpdate);
        fmtPtr("CutDamageMod",         m_gameFuncs.CutDamageMod);
        fmtPtr("UnarmedDamage",        m_gameFuncs.UnarmedDamage);
        fmtPtr("MartialArtsCombat",    m_gameFuncs.MartialArtsCombat);
        fmtPtr("ZoneLoad",             m_gameFuncs.ZoneLoad);
        fmtPtr("ZoneUnload",           m_gameFuncs.ZoneUnload);
        fmtPtr("BuildingPlace",        m_gameFuncs.BuildingPlace);
        fmtPtr("BuildingDestroyed",    m_gameFuncs.BuildingDestroyed);
        fmtPtr("Navmesh",             m_gameFuncs.Navmesh);
        fmtPtr("SpawnCheck",           m_gameFuncs.SpawnCheck);
        fmtPtr("GameFrameUpdate",      m_gameFuncs.GameFrameUpdate);
        fmtPtr("TimeUpdate",           m_gameFuncs.TimeUpdate);
        fmtPtr("SaveGame",             m_gameFuncs.SaveGame);
        fmtPtr("LoadGame",             m_gameFuncs.LoadGame);
        fmtPtr("ImportGame",           m_gameFuncs.ImportGame);
        fmtPtr("CharacterStats",       m_gameFuncs.CharacterStats);
        fmtPtr("InputKeyPressed",      m_gameFuncs.InputKeyPressed);
        fmtPtr("InputMouseMoved",      m_gameFuncs.InputMouseMoved);
        fmtPtr("SquadCreate",          m_gameFuncs.SquadCreate);
        fmtPtr("SquadAddMember",       m_gameFuncs.SquadAddMember);
        fmtPtr("ItemPickup",           m_gameFuncs.ItemPickup);
        fmtPtr("ItemDrop",             m_gameFuncs.ItemDrop);
        fmtPtr("BuyItem",              m_gameFuncs.BuyItem);
        fmtPtr("FactionRelation",      m_gameFuncs.FactionRelation);
        fmtPtr("AICreate",             m_gameFuncs.AICreate);
        fmtPtr("AIPackages",           m_gameFuncs.AIPackages);
        fmtPtr("GunTurret",            m_gameFuncs.GunTurret);
        fmtPtr("GunTurretFire",        m_gameFuncs.GunTurretFire);
        fmtPtr("BuildingDismantle",    m_gameFuncs.BuildingDismantle);
        fmtPtr("BuildingConstruct",    m_gameFuncs.BuildingConstruct);
        fmtPtr("BuildingRepair",       m_gameFuncs.BuildingRepair);
        // ES: Singletons: dirección del global y el valor que contiene ahora mismo.
        // EN: Singletons: address of the global and the value it currently holds.
        spdlog::info("  SGLTN PlayerBase           = 0x{:X} (val=0x{:X})",
                     m_gameFuncs.PlayerBase,
                     m_gameFuncs.PlayerBase ? SEH_ReadPtr(m_gameFuncs.PlayerBase) : 0);
        spdlog::info("  SGLTN GameWorldSingleton   = 0x{:X} (val=0x{:X})",
                     m_gameFuncs.GameWorldSingleton,
                     m_gameFuncs.GameWorldSingleton ? SEH_ReadPtr(m_gameFuncs.GameWorldSingleton) : 0);
        spdlog::info("  Count: {}/{} resolved", m_gameFuncs.CountResolved(), GameFunctions::TotalFunctions());

        // ES: Volcado de los offsets de campo (posición de cada campo dentro de Character y GameWorld);
        //     -1 significa que el offset aún es desconocido.
        // EN: Dump of field offsets (position of each field inside Character and GameWorld);
        //     -1 means the offset is still unknown.
        auto& co = game::GetOffsets().character;
        auto& wo = game::GetOffsets().world;
        spdlog::info("=== OFFSET DUMP ===");
        auto fmtOff = [](const char* name, int val) {
            if (val >= 0)
                spdlog::info("  OFF   {:24s} = 0x{:03X}", name, val);
            else
                spdlog::info("  OFF   {:24s} = -1 (UNKNOWN)", name);
        };
        fmtOff("name", co.name);
        fmtOff("faction", co.faction);
        fmtOff("position", co.position);
        fmtOff("rotation", co.rotation);
        fmtOff("gameDataPtr", co.gameDataPtr);
        fmtOff("inventory", co.inventory);
        fmtOff("stats", co.stats);
        fmtOff("animClassOffset", co.animClassOffset);
        fmtOff("charMovementOffset", co.charMovementOffset);
        fmtOff("writablePosOffset", co.writablePosOffset);
        fmtOff("writablePosVecOffset", co.writablePosVecOffset);
        fmtOff("squad", co.squad);
        fmtOff("equipment", co.equipment);
        fmtOff("isPlayerControlled", co.isPlayerControlled);
        fmtOff("health", co.health);
        fmtOff("healthPartArray", co.healthPartArray);
        fmtOff("healthPartCount", co.healthPartCount);
        fmtOff("healthBase", co.healthBase);
        fmtOff("moneyChain1", co.moneyChain1);
        fmtOff("moneyChain2", co.moneyChain2);
        fmtOff("moneyBase", co.moneyBase);
        fmtOff("sceneNode", co.sceneNode);
        fmtOff("aiPackage", co.aiPackage);
        fmtOff("world.gameSpeed", wo.gameSpeed);
        fmtOff("world.characterList", wo.characterList);
        fmtOff("world.zoneManager", wo.zoneManager);
        fmtOff("world.timeOfDay", wo.timeOfDay);
        spdlog::info("=== END DUMP ===");
    }

    // ES: ═══ Inicializar el SDK (estado del juego por polling) ═══
    // ═══ Initialize SDK (polling-based game state) ═══
    if (m_sdk.Initialize()) {
        m_nativeHud.LogStep("SDK", "KenshiSDK initialized (polling ready)");
    } else {
        m_nativeHud.LogStep("WARN", "KenshiSDK init failed — polling unavailable");
    }

    // ES: ═══ Inicializar VisualProxy (seguimiento del estado de los jugadores remotos) ═══
    // ═══ Initialize VisualProxy (remote player state tracking) ═══
    m_visualProxy.Initialize();
    m_nativeHud.LogStep("SDK", "VisualProxy initialized (state tracking active)");

    m_nativeHud.LogStep("GAME", "Ready! Press F1 for multiplayer menu");
    OutputDebugStringA("KMP: === Core::OnGameLoaded() COMPLETE ===\n");
}

// ES: Envoltorio SEH del update del hilo de red — debe ser una función plana (sin objetos C++ con
//     destructor). Devuelve 0 si fue bien o el código de excepción si petó.
// EN: Returns 0 on success or the exception code if it crashed.
// SEH wrapper for network thread update — must be a plain function (no C++ objects with destructors)
static int SEH_NetworkUpdate(NetworkClient* client) {
    __try {
        client->Update();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

// ES: Bucle del hilo de red: llama a m_client.Update() (ENet) cada ~1 ms mientras m_running. Si
//     Update() peta, se registra (máx. 20 seguidos) y se espera 50 ms para no entrar en bucle de crashes.
// EN: Network thread loop: calls m_client.Update() (ENet) every ~1 ms while m_running. If Update()
//     crashes it is logged (max 20 in a row) and it backs off 50 ms to avoid a tight crash loop.
void Core::NetworkThreadFunc() {
    spdlog::info("Network thread started");
    int consecutiveCrashes = 0;

    while (m_running) {
        int exCode = SEH_NetworkUpdate(&m_client);
        if (exCode != 0) {
            consecutiveCrashes++;
            if (consecutiveCrashes <= 20) {
                spdlog::error("NetworkThread: SEH crash #{} in Update() — exception 0x{:08X}",
                              consecutiveCrashes, static_cast<unsigned>(exCode));
                OutputDebugStringA("KMP: NetworkThread SEH crash in Update()\n");
            }
            // Back off briefly after a crash to avoid tight crash loops
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else {
            consecutiveCrashes = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    spdlog::info("Network thread stopped");
}

// ES: Tras conectar: recorre los personajes existentes, registra y envía al servidor SOLO los de la
//     facción del jugador (miembros del escuadrón) con C2S_EntitySpawnReq fiable, incluyendo estado
//     extendido (salud de 7 partes del cuerpo + vivo). Como efecto secundario descubre el offset
//     isPlayerControlled comparando un char del jugador con un NPC.
// EN: After connecting: walks the existing characters, registers and sends to the server ONLY those
//     of the player faction (squad members) with a reliable C2S_EntitySpawnReq, including extended
//     state (health of 7 body parts + alive). As a side effect it discovers the isPlayerControlled
//     offset by comparing a player char with an NPC.
void Core::SendExistingEntitiesToServer() {
    // After connecting, scan existing characters and register+send them.
    // IMPORTANT: Only send characters from the player's faction (squad members).
    // Uses CharacterIterator which was already validated by PollForGameLoad.

    int count = 0;
    int skippedFaction = 0;

    // ── [DIAG] (sonda v2) ──
    // El volcado de diagnóstico de facción YA NO se dispara aquí: en este punto el
    // personaje del jugador todavía no existe (PlayerBase no resuelto), agotando el
    // contador antes de tiempo. Ahora se dispara desde DiagTickPump() en OnGameTick,
    // cuando el PlayerBase ya es un puntero de heap válido. (Ver game_character.cpp.)
    // EN: ── [DIAG] (probe v2) ── The faction diagnostic dump is NO LONGER fired here: at this point
    //     the player character does not exist yet (PlayerBase unresolved), which exhausted the counter
    //     too early. It now fires from DiagTickPump() in OnGameTick, once PlayerBase is a valid heap
    //     pointer. (See game_character.cpp.)

    // ── Resolve faction ──
    // FUENTE PRIMARIA: getter directo GameWorld -> player(+0x580) -> participant(+0x2A0).
    // No depende de la lista de personajes ni del iterador, así que resuelve la facción
    // aunque la lista aún no esté poblada (causa del bug faction=0x0).
    // EN: ── Resolve faction ── PRIMARY SOURCE: direct getter GameWorld -> player(+0x580) ->
    //     participant(+0x2A0). It does not depend on the character list or the iterator, so it resolves
    //     the faction even when the list is not populated yet (cause of the faction=0x0 bug).
    uintptr_t playerFaction = game::GetPlayerFactionDirect();
    if (playerFaction != 0) {
        // Log del nombre de la facción (Faction+0x1A8 = std::string) para verificar en runtime.
        // EN: Log the faction name (Faction+0x1A8 = std::string) to verify it at runtime.
        const int nameOff = game::GetOffsets().factionExtra.nameStr; // 0x1A8
        std::string facName = SpawnManager::ReadKenshiString(playerFaction + nameOff);
        spdlog::info("Core: Faccion del jugador (directa) = 0x{:X} name='{}'",
                     playerFaction, facName);
    } else {
        // Fallback: la facción capturada antes desde el primer personaje del jugador.
        // EN: Fallback: the faction captured earlier from the first player character.
        playerFaction = m_playerController.GetLocalFactionPtr();
    }

    // ES: ── Recorrido con CharacterIterator ──
    // ── CharacterIterator ──
    game::CharacterIterator iter;
    bool iteratorWorked = (iter.Count() > 0);

    uintptr_t firstPlayerCharPtr = 0;
    uintptr_t firstNpcCharPtr = 0;

    if (iteratorWorked) {
        // ES: Si la facción es desconocida, usar entity_hooks::RevalidateFaction, que hace un escaneo
        //     multi-fuente (nombre + flag isPlayerFaction + votación por frecuencia).
        // If faction unknown, use entity_hooks::RevalidateFaction which does a
        // multi-source scan (name match + isPlayerFaction flag + frequency voting).
        if (playerFaction == 0) {
            if (entity_hooks::RevalidateFaction()) {
                playerFaction = m_playerController.GetLocalFactionPtr();
                if (playerFaction == 0) playerFaction = entity_hooks::GetEarlyPlayerFaction();
                if (playerFaction != 0) {
                    spdlog::info("Core: Faction discovered via RevalidateFaction: 0x{:X}", playerFaction);
                }
            }
        }

        if (playerFaction != 0) {
            spdlog::info("Core: Scanning {} characters via CharacterIterator (faction=0x{:X})...",
                         iter.Count(), playerFaction);

            // ES: Por cada personaje válido con posición no nula: si no es de la facción del jugador se salta
            //     (y se guarda el primero como NPC de referencia); si lo es y no está registrado, se registra
            //     como NPC propio y se envía al servidor.
            // EN: For each valid character with a non-zero position: if it is not in the player faction it is
            //     skipped (the first one is kept as the reference NPC); if it is and is not registered yet, it
            //     is registered as our own NPC and sent to the server.
            while (iter.HasNext()) {
                game::CharacterAccessor character = iter.Next();
                if (!character.IsValid()) continue;

                Vec3 pos = character.GetPosition();
                if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) continue;

                uintptr_t charFaction = character.GetFactionPtr();
                if (charFaction != playerFaction) {
                    skippedFaction++;
                    if (firstNpcCharPtr == 0) firstNpcCharPtr = character.GetPtr();
                    continue;
                }

                if (firstPlayerCharPtr == 0) firstPlayerCharPtr = character.GetPtr();

                void* gameObj = reinterpret_cast<void*>(character.GetPtr());
                if (m_entityRegistry.GetNetId(gameObj) != INVALID_ENTITY) continue;

                EntityID netId = m_entityRegistry.Register(gameObj, EntityType::NPC, m_localPlayerId);
                m_entityRegistry.UpdatePosition(netId, pos);

                Quat rot = character.GetRotation();
                m_entityRegistry.UpdateRotation(netId, rot);

                uintptr_t factionPtr = character.GetFactionPtr();
                uint32_t factionId = 0;
                const int fIdOff = game::GetOffsets().faction.id;
                if (factionPtr != 0 && fIdOff >= 0)
                    Memory::Read(factionPtr + fIdOff, factionId);

                std::string charName = character.GetName();

                PacketWriter writer;
                writer.WriteHeader(MessageType::C2S_EntitySpawnReq);
                writer.WriteU32(netId);
                writer.WriteU8(static_cast<uint8_t>(EntityType::NPC));
                writer.WriteU32(m_localPlayerId);
                writer.WriteU32(0);
                writer.WriteF32(pos.x);
                writer.WriteF32(pos.y);
                writer.WriteF32(pos.z);
                writer.WriteU32(rot.Compress());
                writer.WriteU32(factionId);
                uint16_t nameLen = static_cast<uint16_t>(std::min<size_t>(charName.size(), 255));
                writer.WriteU16(nameLen);
                if (nameLen > 0) writer.WriteRaw(charName.data(), nameLen);

                // ES: Estado extendido: salud por parte del cuerpo + flag de vivo, para que el servidor no arranque
                //     suponiendo 100% en todas las partes.
                // Extended state: per-limb health + alive flag
                // This ensures the server starts with correct health state rather
                // than defaulting to 100% for all limbs.
                writer.WriteU8(1); // hasExtendedState flag
                for (int bp = 0; bp < 7; bp++) {
                    float h = character.GetHealth(static_cast<BodyPart>(bp));
                    writer.WriteF32(h);
                }
                writer.WriteU8(character.IsAlive() ? 1 : 0);

                m_client.SendReliable(writer.Data(), writer.Size());
                count++;
            }
        }
    }

    // ES: Descubrir el offset isPlayerControlled comparando un char del jugador con uno NPC.
    // Discover isPlayerControlled offset by comparing a player char vs NPC char
    if (firstPlayerCharPtr != 0 && firstNpcCharPtr != 0) {
        game::ProbePlayerControlledOffset(firstPlayerCharPtr, firstNpcCharPtr);
    }

    if (count == 0) {
        spdlog::warn("Core: CharacterIterator found 0 squad characters (iterator={}, faction=0x{:X})",
                     iteratorWorked ? "ok" : "FAILED", playerFaction);
    }

    spdlog::info("Core: Sent {} squad characters to server (skipped {} non-squad NPCs)",
                 count, skippedFaction);

    if (count > 0) {
        m_overlay.AddSystemMessage("Syncing " + std::to_string(count) + " squad characters...");
    }
}

// ES: RECLAMAR PERSONAJES DEL MOD. Tras cargar, los personajes "Player 1".."Player 16" de
//     kenshi-online.mod ya existen en el mundo. En vez de llamar a FactoryCreate (poco fiable), se
//     buscan por nombre y se asignan a los jugadores de red: tu slot → entidad local (la controlas
//     tú); slots de otros jugadores → entidad remota (la controla la red, IA suprimida).
// ═══════════════════════════════════════════════════════════════════════════
//  MOD CHARACTER CLAIMING
//  After game load, the kenshi-online.mod's "Player 1" through "Player 16"
//  characters already exist in the world. Instead of calling FactoryCreate
//  (unreliable), we find them by name and assign them to network players.
//
//  - Your assigned slot → local entity (you control it)
//  - Other connected players' slots → remote entity (network controls, AI suppressed)
// ═══════════════════════════════════════════════════════════════════════════

// ES: Lee el nombre del personaje a un buffer C. SpawnManager::ReadKenshiString ya lleva SEH dentro,
//     así que aquí no hace falta __try. Devuelve la longitud copiada (0 si no hay nombre).
// EN: Returns the copied length (0 if there is no name).
// Read character name into a C buffer. Uses SpawnManager::ReadKenshiString
// which handles SEH internally. No __try needed at this level.
static size_t ReadCharNameSafe(uintptr_t charPtr, char* outBuf, size_t bufSize) {
    outBuf[0] = '\0';
    auto& offsets = game::GetOffsets().character;
    uintptr_t nameAddr = charPtr + offsets.name;
    std::string name = SpawnManager::ReadKenshiString(nameAddr);
    if (name.empty()) return 0;
    size_t copyLen = (name.size() < bufSize - 1) ? name.size() : bufSize - 1;
    memcpy(outBuf, name.c_str(), copyLen);
    outBuf[copyLen] = '\0';
    return copyLen;
}

// ES: Recorre los personajes buscando "Player N" (N = 1..16). El de tu slot, si el motor confirma
//     que está en la lista nativa del jugador (PI+0x2B0), se desplaza en un círculo de 3 m según el
//     slot, se registra como PlayerCharacter LOCAL y se envía al servidor con estado extendido. Los
//     de otros slots solo se registran en el log: el enlace remoto real lo hace HandleEntitySpawn
//     cuando llega S2C_EntitySpawn. Nota: remoteClaimed nunca se incrementa (siempre registra 0).
// EN: Walks the characters looking for "Player N" (N = 1..16). The one for your slot, if the engine
//     confirms it is in the native player list (PI+0x2B0), is moved onto a 3 m circle by slot,
//     registered as a LOCAL PlayerCharacter and sent to the server with extended state. Other slots
//     are only logged: the real remote link is done by HandleEntitySpawn when S2C_EntitySpawn
//     arrives. Note: remoteClaimed is never incremented (it always logs 0).
void Core::FindAndClaimModCharacters() {
    int mySlot = m_lobbyManager.GetPlayerSlot();
    if (mySlot <= 0) {
        spdlog::warn("Core: FindAndClaimModCharacters — no player slot assigned");
        return;
    }

    game::CharacterIterator iter;
    if (iter.Count() == 0) {
        spdlog::warn("Core: FindAndClaimModCharacters — CharacterIterator empty");
        return;
    }

    spdlog::info("Core: Scanning {} characters for mod player templates (my slot={})...",
                 iter.Count(), mySlot);

    int localClaimed = 0;
    int remoteClaimed = 0;
    std::string myExpectedName = "Player " + std::to_string(mySlot);

    while (iter.HasNext()) {
        game::CharacterAccessor character = iter.Next();
        if (!character.IsValid()) continue;

        // ES: Leer el nombre del personaje.
        // Read character name
        char nameBuf[64] = {};
        ReadCharNameSafe(character.GetPtr(), nameBuf, sizeof(nameBuf));
        std::string charName(nameBuf);

        // ES: ¿Es un personaje del mod ("Player 1" a "Player 16")? Si no, siguiente.
        // Check if this is a mod player character ("Player 1" through "Player 16")
        if (charName.size() < 8 || charName.substr(0, 7) != "Player ") continue;

        // ES: Sacar el número de slot del nombre.
        // Parse the slot number
        int charSlot = 0;
        try { charSlot = std::stoi(charName.substr(7)); }
        catch (...) { continue; }
        if (charSlot < 1 || charSlot > 16) continue;

        Vec3 pos = character.GetPosition();
        Quat rot = character.GetRotation();
        void* gameObj = reinterpret_cast<void*>(character.GetPtr());

        // ES: Saltar si ya está registrado.
        // Skip if already registered
        if (m_entityRegistry.GetNetId(gameObj) != INVALID_ENTITY) {
            spdlog::debug("Core: '{}' already registered, skipping", charName);
            continue;
        }

        if (charSlot == mySlot) {
            // ── This is MY character ──
            // [FIX-GHOST 2026-07] Antes de reclamar como entidad LOCAL DEL JUGADOR,
            // verificar contra la fuente de verdad del motor (PI+0x2B0): un NPC
            // fantasma del mundo puede llamarse "Player N" sin ser del jugador.
            // Reclamarlo desviaba TODOS los fixes del host (facción, platoon,
            // hostilidad) al fantasma y congelaba al personaje real.
            //   1 = confirmado del jugador → reclamar.
            //   0 = NPC fantasma → ignorar (sigue siendo entidad normal del mundo).
            //  -1 = lista aún no disponible → no reclamar todavía; el scan se
            //       reintenta (45 intentos) y ClaimHostPrimaryCharacter cubre el hueco.
            // EN: ── This is MY character ──
            //     [FIX-GHOST 2026-07] Before claiming it as the PLAYER LOCAL entity, check it against the engine
            //     source of truth (PI+0x2B0, the native player character list): a world ghost NPC can be named
            //     "Player N" without belonging to the player. Claiming it diverted ALL host fixes (faction,
            //     platoon, hostility) to the ghost and froze the real character.
            //       1 = confirmed player char → claim.
            //       0 = ghost NPC → ignore (it remains a normal world entity).
            //      -1 = list not available yet → do not claim yet; the scan retries (45 attempts) and
            //           ClaimHostPrimaryCharacter covers the gap.
            int inPlayerList = game::IsInPlayerCharactersList(character.GetPtr());
            if (inPlayerList != 1) {
                if (inPlayerList == 0) {
                    spdlog::warn("Core: '{}' encaja con 'Player {}' pero NO está en PI+0x2B0 "
                                 "(lista nativa del jugador) — es un NPC fantasma del mundo, "
                                 "NO se reclama como personaje del jugador", charName, charSlot);
                } else {
                    spdlog::info("Core: '{}' encaja con 'Player {}' pero la lista PI+0x2B0 aún "
                                 "no está disponible — claim aplazado al siguiente intento",
                                 charName, charSlot);
                }
                continue;
            }

            // ES: Desplazar la posición para que los jugadores no aparezcan unos dentro de otros: círculo de 3 m
            //     de radio según el número de slot.
            // Offset position so players don't spawn inside each other.
            // Arrange in a circle with 3m radius based on slot number.
            float angle = static_cast<float>(charSlot - 1) * (6.2831853f / 16.f); // 2*PI / 16
            Vec3 offset{std::cos(angle) * 3.f, 0.f, std::sin(angle) * 3.f};
            Vec3 spawnPos{pos.x + offset.x, pos.y, pos.z + offset.z};
            character.WritePosition(spawnPos);
            pos = spawnPos;

            EntityID netId = m_entityRegistry.Register(gameObj, EntityType::PlayerCharacter,
                                                        m_localPlayerId);
            m_entityRegistry.UpdatePosition(netId, pos);
            m_entityRegistry.UpdateRotation(netId, rot);

            // ES: Enviar al servidor (C2S_EntitySpawnReq con id de facción y estado extendido).
            // Send to server
            uintptr_t factionPtr = character.GetFactionPtr();
            uint32_t factionId = 0;
            {
                const int fIdOff = game::GetOffsets().faction.id;
                if (factionPtr != 0 && fIdOff >= 0)
                    Memory::Read(factionPtr + fIdOff, factionId);
            }

            PacketWriter writer;
            writer.WriteHeader(MessageType::C2S_EntitySpawnReq);
            writer.WriteU32(netId);
            writer.WriteU8(static_cast<uint8_t>(EntityType::PlayerCharacter));
            writer.WriteU32(m_localPlayerId);
            writer.WriteU32(0); // templateId
            writer.WriteF32(pos.x); writer.WriteF32(pos.y); writer.WriteF32(pos.z);
            writer.WriteU32(rot.Compress());
            writer.WriteU32(factionId);
            writer.WriteString(charName);

            // Extended state
            writer.WriteU8(1);
            for (int bp = 0; bp < 7; bp++)
                writer.WriteF32(character.GetHealth(static_cast<BodyPart>(bp)));
            writer.WriteU8(character.IsAlive() ? 1 : 0);

            m_client.SendReliable(writer.Data(), writer.Size());
            localClaimed++;

            spdlog::info("Core: CLAIMED '{}' as LOCAL entity {} (pos={:.0f},{:.0f},{:.0f})",
                         charName, netId, pos.x, pos.y, pos.z);

        } else {
            // ES: ── Pertenece a otro jugador (o slot libre) ── Cuando el servidor mande S2C_EntitySpawn de ese
            //     jugador, HandleEntitySpawn encontrará y enlazará este objeto. Aquí solo se registra en el log.
            // ── This belongs to another player (or unclaimed slot) ──
            // Register as remote entity. When the server sends S2C_EntitySpawn for
            // this player, HandleEntitySpawn will find and link this game object.
            // For now, store the mapping: slot → game object pointer.
            // The actual remote registration happens when the server tells us about it.
            spdlog::info("Core: Found mod character '{}' (slot {}) — available for remote player",
                         charName, charSlot);
        }
    }

    spdlog::info("Core: Mod character scan complete — claimed {} local, {} available remote",
                 localClaimed, remoteClaimed);

    if (localClaimed > 0) {
        m_nativeHud.AddSystemMessage("Found your character: Player " + std::to_string(mySlot));
        m_initialEntityScanDone = true;
    }
}

// ── ClaimHostPrimaryCharacter ──
// Reclama el personaje PRIMARIO del host por la cadena directa del motor
// (GameWorld+0x580 -> +0x2B0 -> data[0]), SIN depender del nombre "Player N".
//
// MOTIVO (flujo connected-then-load, bug entities=0 / tracked:0):
//   Cuando el jugador conecta DESDE EL MENÚ y LUEGO carga el save, su personaje se crea
//   DURANTE la carga, con el hook CharacterCreate en passthrough (se reactiva en "full mode"
//   DESPUÉS, post-load). Por eso el mod NO captura su creación -> entities=0. Y el char del
//   host conserva el nombre del save (no "Player N"), así que FindAndClaimModCharacters (que
//   filtra por "Player N") tampoco lo encuentra. Esta función cierra ese hueco: localiza el
//   char por la lista REAL del jugador del motor y lo registra/envía como entidad LOCAL.
//
// Devuelve true si quedó reclamado (o ya lo estaba). Idempotente: si ya está registrado, no
// duplica. Diseñada para llamarse cada tick hasta éxito (la lista puede tardar en poblarse).
// EN: ── ClaimHostPrimaryCharacter ──
//     Claims the host PRIMARY character through the engine direct chain
//     (GameWorld+0x580 -> +0x2B0 -> data[0]), WITHOUT relying on the "Player N" name.
//     REASON (connected-then-load flow, entities=0 / tracked:0 bug): when the player connects FROM
//     THE MENU and THEN loads the save, their character is created DURING the load, with the
//     CharacterCreate hook in passthrough (it goes back to "full mode" AFTER, post-load). So the mod
//     does NOT capture its creation -> entities=0. And the host char keeps the save name (not
//     "Player N"), so FindAndClaimModCharacters (which filters by "Player N") does not find it
//     either. This function closes that gap: it locates the char via the engine REAL player list
//     and registers/sends it as a LOCAL entity.
//     Returns true if it ended up claimed (or already was). Idempotent: does not duplicate if already
//     registered. Meant to be called every tick until it succeeds (the list may take a while).
bool Core::ClaimHostPrimaryCharacter() {
    // Solo el host reclama su char primario por esta vía (el joiner recibe su char por spawn).
    // Gate por slot: el host es slot 1 normalmente; aceptamos cualquier slot > 0 asignado.
    // EN: Only the host claims its primary char this way (the joiner gets its char via spawn).
    //     Slot gate: the host is normally slot 1; any assigned slot > 0 is accepted.
    int mySlot = m_lobbyManager.GetPlayerSlot();

    // 1) Localiza el char primario por la cadena directa (robusta, sin nombre).
    // EN: 1) Locate the primary char through the direct chain (robust, name-independent).
    uintptr_t primary = game::GetPlayerPrimaryCharacterDirect();
    if (primary == 0) {
        // Lista aún sin poblar (timing post-load) — el caller reintenta el próximo tick.
        return false;
    }

    void* gameObj = reinterpret_cast<void*>(primary);

    // 2) Idempotencia: si ya está registrado, nada que hacer.
    // EN: 2) Idempotency: if already registered, nothing to do.
    if (m_entityRegistry.GetNetId(gameObj) != INVALID_ENTITY) {
        m_initialEntityScanDone = true;
        return true;
    }

    game::CharacterAccessor character(gameObj);
    if (!character.IsValid()) return false;

    Vec3 pos = character.GetPosition();
    Quat rot = character.GetRotation();

    // 3) Registrar como entidad LOCAL del jugador.
    // EN: 3) Register it as the player LOCAL entity.
    EntityID netId = m_entityRegistry.Register(gameObj, EntityType::PlayerCharacter, m_localPlayerId);
    m_entityRegistry.UpdatePosition(netId, pos);
    m_entityRegistry.UpdateRotation(netId, rot);

    // 4) Resolver faction id (para el server).
    // EN: 4) Resolve the faction id (for the server).
    uintptr_t factionPtr = character.GetFactionPtr();
    uint32_t factionId = 0;
    {
        const int fIdOff = game::GetOffsets().faction.id;
        if (factionPtr != 0 && fIdOff >= 0)
            Memory::Read(factionPtr + fIdOff, factionId);
    }

    // Nombre real del char (para el server / UI). Puede ser el del save (no "Player N").
    // EN: Real char name (for server / UI). May be the save name (not "Player N").
    std::string charName = character.GetName();
    if (charName.empty())
        charName = "Player " + std::to_string(mySlot > 0 ? mySlot : 1);

    // 5) Anunciar el spawn al servidor (mismo formato que FindAndClaimModCharacters).
    // EN: 5) Announce the spawn to the server (same format as FindAndClaimModCharacters).
    PacketWriter writer;
    writer.WriteHeader(MessageType::C2S_EntitySpawnReq);
    writer.WriteU32(netId);
    writer.WriteU8(static_cast<uint8_t>(EntityType::PlayerCharacter));
    writer.WriteU32(m_localPlayerId);
    writer.WriteU32(0); // templateId
    writer.WriteF32(pos.x); writer.WriteF32(pos.y); writer.WriteF32(pos.z);
    writer.WriteU32(rot.Compress());
    writer.WriteU32(factionId);
    writer.WriteString(charName);
    writer.WriteU8(1);
    for (int bp = 0; bp < 7; bp++)
        writer.WriteF32(character.GetHealth(static_cast<BodyPart>(bp)));
    writer.WriteU8(character.IsAlive() ? 1 : 0);
    m_client.SendReliable(writer.Data(), writer.Size());

    m_initialEntityScanDone = true;
    m_nativeHud.AddSystemMessage("Host character claimed: " + charName);
    spdlog::info("Core: ClaimHostPrimaryCharacter — CLAIMED host primary char 0x{:X} '{}' as LOCAL entity {} "
                 "(pos={:.0f},{:.0f},{:.0f}, faction=0x{:X} id={})",
                 primary, charName, netId, pos.x, pos.y, pos.z, factionPtr, factionId);

    // [FIX-PLATOON] RE-ARME al RECLAMAR el host: este es el momento real en que el host
    // primario existe en el mundo. Hasta ahora ResetHostPlatoonFix() solo se llamaba en
    // recarga de save GameReady (línea ~1325) y en nueva conexión (~7527), NUNCA en la
    // PRIMERA carga desde menú (crear personaje). Re-armar aquí (s_platoonAttempts=0,
    // done=false) garantiza que el fix arranque limpio justo cuando hay un host al que
    // aplicarlo, sin depender de la fase previa. Coexiste con el resto de re-armes.
    // EN: [FIX-PLATOON] RE-ARM when CLAIMING the host: this is the real moment the host primary exists
    //     in the world. Until now ResetHostPlatoonFix() was only called on a GameReady save reload and on
    //     a new connection, NEVER on the FIRST load from the menu (character creation). Re-arming here
    //     (s_platoonAttempts=0, done=false) guarantees the fix starts clean right when there is a host to
    //     apply it to, regardless of the previous phase. Coexists with the other re-arms.
    //     (The "line ~1325" / "~7527" references are approximate and out of date.)
    // ES: (Las referencias "línea ~1325" / "~7527" son aproximadas y están desfasadas.)
    ResetHostPlatoonFix();
    spdlog::info("[FIX-PLATOON] re-armado al reclamar el host (char=0x{:X}) — contador a 0, "
                 "listo para aplicar el re-enlace AI<->platoon.", primary);

    return true;
}

// ES: Busca un personaje del mod por nombre de slot (p.ej. slot 2 → "Player 2") recorriendo
//     CharacterIterator. Devuelve el puntero al objeto del juego o nullptr.
// Find a mod character by slot name (e.g. slot 2 → "Player 2").
// Returns the game object pointer if found, or nullptr.
void* Core::FindModCharacterBySlot(int slot) {
    if (slot < 1 || slot > 16) return nullptr;

    std::string targetName = "Player " + std::to_string(slot);

    game::CharacterIterator iter;
    while (iter.HasNext()) {
        game::CharacterAccessor character = iter.Next();
        if (!character.IsValid()) continue;

        char nameBuf[64] = {};
        ReadCharNameSafe(character.GetPtr(), nameBuf, sizeof(nameBuf));
        if (std::string(nameBuf) == targetName) {
            return reinterpret_cast<void*>(character.GetPtr());
        }
    }
    return nullptr;
}

// ES: ── Estáticos del escaneo de entidades (ámbito de fichero para poder reiniciarlos desde otra
//     función) ── Los usa OnGameTick para los reintentos y se reinician en HandleSpawnQueue al reconectar.
// ── Entity scan statics (file scope for cross-function reset) ──
// Used in OnGameTick for retry logic, reset in HandleSpawnQueue on reconnect.
static int s_entityScanRetries = 0;
static bool s_wasScanning = false;

// ES: Temporizador de keepalive — se reinicia al reconectar con ResetKeepaliveTimer().
// Keepalive timer — reset on reconnect via ResetKeepaliveTimer().
static auto s_lastKeepalive = std::chrono::steady_clock::now();

// ES: Reinicia el temporizador de keepalive (el primero sale 5 s después de conectar).
// EN: Resets the keepalive timer (the first one goes out 5 s after connecting).
void ResetKeepaliveTimer() {
    s_lastKeepalive = std::chrono::steady_clock::now();
}

// ── Fix de facción del personaje del HOST ──
// Estado one-shot (re-armable en disconnect vía ResetHostFactionFix()).
//   s_hostFactionFixed: true cuando al menos un char local quedó con faction == player faction.
//   s_hostFactionAttempts: limita los reintentos (el char puede tardar varios ticks en existir).
// EN: ── HOST character faction fix ──
//     One-shot state (re-armable on disconnect via ResetHostFactionFix()).
//       s_hostFactionFixed: true once at least one local char has faction == player faction.
//       s_hostFactionAttempts: caps the retries (the char may take several ticks to exist).
static std::atomic<bool> s_hostFactionFixed{false};
static int s_hostFactionAttempts = 0;
static auto s_lastHostFactionTry = std::chrono::steady_clock::now();
static constexpr int HOST_FACTION_MAX_ATTEMPTS = 120; // ~varios segundos de reintentos

// ES: Re-arma el fix de facción del host (disconnect / nueva carga).
// EN: Re-arms the host faction fix (disconnect / new load).
void ResetHostFactionFix() {
    s_hostFactionFixed.store(false);
    s_hostFactionAttempts = 0;
    s_lastHostFactionTry = std::chrono::steady_clock::now();
}

// Orquesta el fix: resuelve la PLAYER faction (GameWorld+0x580 -> +0x2A0) y la escribe
// en char+0x10 de TODOS los GameObjects locales del host cuya facción no coincida.
// Throttle de ~250ms reales entre intentos; one-shot cuando al menos uno queda correcto.
// Llamado desde OnGameTick (ambas ramas), SOLO en el host.
// EN: Drives the fix: resolves the PLAYER faction (GameWorld+0x580 -> +0x2A0) and writes it into
//     char+0x10 (the Character faction pointer) of EVERY local host GameObject whose faction does not
//     match. ~250 ms real-time throttle between attempts; one-shot once at least one is correct.
//     Called from OnGameTick (both branches), ONLY on the host.
static void FixHostCharacterFactionTick(Core& core) {
    if (s_hostFactionFixed.load()) return;                 // ya arreglado
    if (s_hostFactionAttempts >= HOST_FACTION_MAX_ATTEMPTS) return; // nos rendimos (log una vez abajo)

    // Throttle: no martillear cada frame.
    auto now = std::chrono::steady_clock::now();
    if (now - s_lastHostFactionTry < std::chrono::milliseconds(250)) return;
    s_lastHostFactionTry = now;
    s_hostFactionAttempts++;

    // 1) Fuente de verdad: la player faction directa (la que resuelve 'Sinnombre').
    //    Fallback: la facción capturada por el PlayerController (votación multi-fuente).
    // EN: 1) Source of truth: the direct player faction (the one that resolves 'Sinnombre'/Nameless).
    //        Fallback: the faction captured by PlayerController (multi-source voting).
    uintptr_t playerFaction = game::GetPlayerFactionDirect();
    if (playerFaction == 0) {
        playerFaction = core.GetPlayerController().GetLocalFactionPtr();
    }
    if (playerFaction == 0) {
        // Aún no hay facción del jugador resoluble — reintentar en el próximo throttle.
        if (s_hostFactionAttempts == HOST_FACTION_MAX_ATTEMPTS) {
            spdlog::warn("[DIAG-FAC] Host faction fix: sin player faction tras {} intentos — abortado",
                         s_hostFactionAttempts);
        }
        return;
    }

    // 2) Iterar los GameObjects locales del host y arreglar el que tenga faction != player.
    // EN: 2) Walk the host local GameObjects and fix the one whose faction != player.
    auto& registry = core.GetEntityRegistry();
    auto localEntities = registry.GetPlayerEntities(core.GetLocalPlayerId());
    if (localEntities.empty()) return; // el char del host aún no está registrado

    bool anyCorrect = false;
    int checked = 0;
    for (EntityID eid : localEntities) {
        void* gameObj = registry.GetGameObject(eid);
        if (!gameObj) continue;
        checked++;

        game::FixFactionResult res = game::FixCharacterFactionTo(gameObj, playerFaction);
        if (res == game::FixFactionResult::Fixed ||
            res == game::FixFactionResult::AlreadyCorrect) {
            anyCorrect = true;
        }
    }

    if (anyCorrect) {
        s_hostFactionFixed.store(true);
        spdlog::info("[DIAG-FAC] Host faction fix COMPLETO — player faction 0x{:X} aplicada "
                     "({} chars locales revisados, intento {})",
                     playerFaction, checked, s_hostFactionAttempts);
        core.GetNativeHud().AddSystemMessage("Faccion del jugador corregida (combate habilitado)");
    }
}

// ── [FIX-SIMSEED] Desbloqueo del AI tick de combate del HOST: seed de char+0xD0 ──────
// CAUSA RAÍZ confirmada por RE de bytes (Fase 4, 2026-06-18, doble verificación):
//   El AI tick pesado [vtbl+0xE8] = 0x5CCD90 SÍ se invoca sobre el char del host (round-robin
//   refutado con N=96: 8 chars/frame, el cursor 0x2132ED0 barre las 96 posiciones cada ~12
//   frames, así que la pos ~57 del host recibe la llamada varias veces por segundo). El bug
//   está DENTRO de 0x5CCD90, en la comparación de "horas de simulación pendientes":
//     0x5CCE3A  movss  xmm3,[char+0xD0]          ; lastProcessed (horas de juego)
//     0x5CCE6B  subss  xmm0,xmm3                 ; diff = relojSim - char+0xD0
//     0x5CCE6F  comiss xmm0,[0x16B3BFC=12.0]     ; diff vs 12.0
//     0x5CCE76  jbe 0x5CCECC                     ; diff<=12 → COMBATE NORMAL
//             else → 0x5CCE78 rama CLEANUP ("corpse decayed/unloaded") → jmp 0x5CD284 epílogo.
//   La rama CLEANUP (diff>12) NO escribe char+0xD0 (verificado byte a byte: el único write de
//   +0xD0 está en 0x5CCE4D auto-init y 0x5CCEC4 rama combate). Con un char recién reclamado por
//   el mod char+0xD0 == 0.0 y el reloj ya va por 35.17h → diff=35.17 > 12 → SIEMPRE cleanup,
//   nunca combate, y +0xD0 se queda clavado en 0.0 PARA SIEMPRE (bucle infinito de cleanup).
//   Síntoma exacto observado en runtime: char+0xD0=0.000000 persistente, sin atacar/levantarse.
//   Existe una auto-init nativa (0x5CCE4D: if +0xD0==0 → +0xD0=reloj → combate) pero está detrás
//   del gate `cmp byte[char+0x5BC],0; je 0x5CD1C0` (0x5CCE2B): si el char reclamado tiene
//   +0x5BC==0, esa auto-init nunca corre y caemos en cleanup.
//
// FIX (Opción A del audit, la verificada): sembrar char+0xD0 = relojSim (float de 4 bytes) en
//   los chars locales del host. Eso fuerza diff≈0 ≤ 12 → rama combate 0x5CCECC, que a partir de
//   ahí RE-ESCRIBE +0xD0 cada frame (0x5CCEC4 = max(+0xD0, reloj-6)) y se auto-mantiene sola.
//   Es seguro: char+0xD0 es un campo de estado del propio char (no estructura compartida del
//   motor), write de 4 bytes bajo SEH. NO toca el unordered_set de simulación (eso era H1, ya
//   refutada: hostInSimList=YES). One-shot re-armable (como el faction fix), throttled.
//
// Reloj de simulación: SimClock = *(modBase+0x21303D0); reloj(double) en SimClock+0xA0 (horas
//   de juego = día*24 + horaDelDía). 0x5CCE36 lo lee y hace cvtsd2ss → float; por eso +0xD0 se
//   trata como FLOAT de 4 bytes. Sembramos el mismo valor (float) que compara el motor.
// EN: ── [FIX-SIMSEED] Unblocks the HOST combat AI tick: seed of char+0xD0 ──
//     ROOT CAUSE confirmed by byte-level RE (Phase 4, 2026-06-18, double-checked):
//       The heavy AI tick [vtbl+0xE8] = 0x5CCD90 IS invoked on the host char (round-robin refuted
//       with N=96: 8 chars/frame, cursor 0x2132ED0 sweeps the 96 slots every ~12 frames). The bug is
//       INSIDE 0x5CCD90, in the "pending simulation hours" comparison (see listing above):
//       diff = simClock - char+0xD0 (lastProcessed, in game hours); if diff <= 12.0 → NORMAL
//       COMBAT, else → CLEANUP branch ("corpse decayed/unloaded") → epilogue.
//       The CLEANUP branch does NOT write char+0xD0 (the only writes are the auto-init at 0x5CCE4D
//       and the combat branch at 0x5CCEC4). A char freshly claimed by the mod has char+0xD0 == 0.0
//       while the clock is already at e.g. 35.17h → diff > 12 → ALWAYS cleanup, never combat, and
//       +0xD0 stays stuck at 0.0 FOREVER (infinite cleanup loop). The native auto-init only runs
//       behind the gate byte[char+0x5BC] != 0, so a claimed char with +0x5BC==0 never gets it.
//     FIX (audit option A, the verified one): seed char+0xD0 = simClock (4-byte float) on the host
//       local chars. That forces diff≈0 ≤ 12 → combat branch 0x5CCECC, which from then on REWRITES
//       +0xD0 every frame (max(+0xD0, clock-6)) and keeps itself alive. Safe: char+0xD0 is the char
//       own state (not a shared engine structure), 4-byte write under SEH. It does NOT touch the
//       simulation unordered_set (that was hypothesis H1, refuted). One-shot re-armable, throttled.
//     Simulation clock: SimClock = *(modBase+0x21303D0); clock (double) at SimClock+0xA0 (game hours
//       = day*24 + hourOfDay). 0x5CCE36 reads it and converts to float, so +0xD0 is treated as a
//       4-byte FLOAT. We seed the same float value the engine compares.

// ES: Estado one-shot del FIX-SIMSEED (hecho / intentos / último intento) y tope de reintentos.
// EN: FIX-SIMSEED one-shot state (done / attempts / last try) and retry cap.
static std::atomic<bool> s_hostSimSeeded{false};
static int s_hostSimSeedAttempts = 0;
static auto s_lastHostSimSeedTry = std::chrono::steady_clock::now();
static constexpr int HOST_SIMSEED_MAX_ATTEMPTS = 240; // ~varios segundos de reintentos

// ES: Re-arma el FIX-SIMSEED (disconnect / nueva carga).
// EN: Re-arms FIX-SIMSEED (disconnect / new load).
void ResetHostSimSeedFix() {
    s_hostSimSeeded.store(false);
    s_hostSimSeedAttempts = 0;
    s_lastHostSimSeedTry = std::chrono::steady_clock::now();
}

// Lee el reloj de simulación (SimClock+0xA0, double → float) en una pasada SEH POD.
// Devuelve true y *outClock = reloj(float) si el puntero es plausible; false si no se pudo leer.
// EN: Reads the simulation clock (SimClock+0xA0, double → float) in a POD SEH pass.
//     Returns true and *outClock = clock (float) if the pointer is plausible; false if unreadable.
//     RVA 0x21303D0 = global pointer to the SimClock object inside kenshi_x64.exe.
static bool SEH_ReadSimClockFloat(uintptr_t modBase, float* outClock) {
    bool ok = false;
    __try {
        uintptr_t scPtr = 0;
        if (Memory::Read(modBase + 0x21303D0, scPtr) && scPtr > 0x10000) {
            double clk = 0.0;
            if (Memory::Read(scPtr + 0xA0, clk) && clk >= 0.0 && clk < 1.0e9) {
                *outClock = static_cast<float>(clk);
                ok = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

// Igual que SEH_ReadSimClockFloat pero devuelve el reloj como DOUBLE (8 bytes) sin degradar a
// float. Misma resolución del puntero (SimClock = *(modBase+0x21303D0); reloj en SimClock+0xA0),
// pero el timestamp médico char+0x4C0 es un double y el helper nativo 0x791CA0 calcula
// dt = SimClock(double) - char+0x4C0(double) SIN clamp: si sembráramos un float redondeado por
// encima del reloj real, dt saldría negativo (el mismo bug que arreglamos). Por eso aquí se lee
// y se siembra en doble precisión exacta.
// EN: Same as SEH_ReadSimClockFloat but returns the clock as a DOUBLE (8 bytes) without degrading
//     it to float. Same pointer resolution (SimClock = *(modBase+0x21303D0); clock at SimClock+0xA0),
//     but the medical timestamp char+0x4C0 is a double and the native helper 0x791CA0 computes
//     dt = SimClock(double) - char+0x4C0(double) WITHOUT clamping: seeding a float rounded above the
//     real clock would make dt negative (the very bug being fixed). So it is read and seeded in exact
//     double precision.
static bool SEH_ReadSimClockDouble(uintptr_t modBase, double* outClock) {
    bool ok = false;
    __try {
        uintptr_t scPtr = 0;
        if (Memory::Read(modBase + 0x21303D0, scPtr) && scPtr > 0x10000) {
            double clk = 0.0;
            if (Memory::Read(scPtr + 0xA0, clk) && clk >= 0.0 && clk < 1.0e9) {
                *outClock = clk;
                ok = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

// Siembra char+0xD0 (lastProcessed) = relojSim en UN char, SOLO si está "atrasado":
// char+0xD0 == 0.0  ó  diff(reloj - char+0xD0) > 12.0  (la condición exacta que manda el AI
// tick a la rama cleanup en 0x5CCE76). Si ya está dentro de la ventana (diff<=12 y !=0) no
// toca nada (el motor ya lo mantiene). Devuelve: 1=sembrado, 0=ya correcto, -1=no legible.
// POD, sin objetos C++ en el __try (igual que el resto de helpers SEH del archivo).
// EN: Seeds char+0xD0 (lastProcessed) = simClock on ONE char, ONLY if it is "behind":
//     char+0xD0 == 0.0 or diff(clock - char+0xD0) > 12.0 (the exact condition that sends the AI tick
//     to the cleanup branch at 0x5CCE76). If already inside the window (diff<=12 and !=0) nothing is
//     touched (the engine keeps it). Returns: 1=seeded, 0=already correct, -1=unreadable.
//     POD, no C++ objects inside __try (like the other SEH helpers in this file).
static int SEH_SeedCharLastProcessed(uintptr_t charPtr, float simClock,
                                     float* outPrev /*valor previo de +0xD0, para log*/) {
    int result = -1;
    __try {
        float prev = -1.0f;
        if (!Memory::Read(charPtr + 0xD0, prev)) { return -1; }
        *outPrev = prev;
        // Misma condición que el gate del motor (0x5CCE42 ==0.0 / 0x5CCE76 diff>12.0):
        //   prev==0.0  → nunca procesado (cae siempre en cleanup, bucle infinito).
        //   reloj-prev > 12.0 → fuera de ventana → rama cleanup (no escribe +0xD0).
        // EN: Same condition as the engine gate: prev==0.0 → never processed (always cleanup, infinite
        //     loop); clock-prev > 12.0 → out of window → cleanup branch (does not write +0xD0).
        bool stale = (prev == 0.0f) || ((simClock - prev) > 12.0f);
        if (!stale) { result = 0; return 0; }       // ya dentro de ventana → no tocar
        if (Memory::Write(charPtr + 0xD0, simClock)) // seed = reloj → diff≈0 → rama combate
            result = 1;
        else
            result = -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { result = -1; }
    return result;
}

// Orquesta el seed de char+0xD0 para los chars locales del host. Mismo patrón que el faction
// fix: throttle ~250ms, one-shot re-armable, solo host. Llamado desde OnGameTick (ambas ramas).
// Discrimina en log: si tras sembrar el combate arranca → causa confirmada (rama cleanup).
// EN: Drives the char+0xD0 seed for the host local chars. Same pattern as the faction fix:
//     ~250 ms throttle, re-armable one-shot, host only. Called from OnGameTick (both branches).
//     Log discriminates: if combat starts after seeding → cause confirmed (cleanup branch).
static void SeedHostCharLastProcessedTick(Core& core) {
    if (s_hostSimSeeded.load()) return;                          // ya hecho esta partida
    if (s_hostSimSeedAttempts >= HOST_SIMSEED_MAX_ATTEMPTS) return;

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastHostSimSeedTry < std::chrono::milliseconds(250)) return;
    s_lastHostSimSeedTry = now;
    s_hostSimSeedAttempts++;

    // 1) Reloj de simulación actual (lo que el AI tick compara con char+0xD0).
    // EN: 1) Current simulation clock (what the AI tick compares against char+0xD0).
    uintptr_t modBase = Memory::GetModuleBase();
    float simClock = -1.0f;
    if (!SEH_ReadSimClockFloat(modBase, &simClock)) {
        // Aún no hay reloj resoluble → reintentar en el próximo throttle.
        return;
    }
    // Con reloj < 12.0 el problema no se manifiesta (diff nunca pasa de 12 contra 0.0): aun así
    // sembramos si está exactamente en 0.0, no cuesta nada y deja el char "al día" desde ya.
    // EN: With a clock < 12.0 the problem does not show (diff never exceeds 12 vs 0.0): we still seed if
    //     it is exactly 0.0; it costs nothing and brings the char "up to date" right away.

    // 2) Iterar los GameObjects locales del host y sembrar el que esté atrasado.
    // EN: 2) Walk the host local GameObjects and seed the one that is behind.
    auto& registry = core.GetEntityRegistry();
    auto localEntities = registry.GetPlayerEntities(core.GetLocalPlayerId());
    if (localEntities.empty()) return; // el char del host aún no está registrado

    int seeded = 0, alreadyOk = 0, unreadable = 0;
    float lastPrev = -1.0f, lastChar = 0.0f;
    for (EntityID eid : localEntities) {
        void* gameObj = registry.GetGameObject(eid);
        if (!gameObj) continue;
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(gameObj);
        float prev = -1.0f;
        int r = SEH_SeedCharLastProcessed(charPtr, simClock, &prev);
        if (r == 1) { seeded++; lastPrev = prev; lastChar = static_cast<float>(charPtr & 0xFFFFFFFF); }
        else if (r == 0) alreadyOk++;
        else unreadable++;
    }

    // One-shot: nos damos por hechos cuando TODOS los chars locales están dentro de ventana
    // (sembrados o ya correctos) y al menos uno se pudo leer.
    // EN: One-shot: done when ALL local chars are inside the window (seeded or already correct) and at
    //     least one could be read.
    if (unreadable == 0 && (seeded + alreadyOk) > 0) {
        s_hostSimSeeded.store(true);
        spdlog::info("[FIX-SIMSEED] char+0xD0 sembrado en chars del host — simClock={:.4f} "
                     "(sembrados={} yaCorrectos={} prevDelUltimo={:.4f}, intento {}). "
                     "Esto fuerza diff<=12 -> rama combate 0x5CCECC (AI tick desbloqueado).",
                     simClock, seeded, alreadyOk, lastPrev, s_hostSimSeedAttempts);
        if (seeded > 0)
            core.GetNativeHud().AddSystemMessage("Simulacion de combate desbloqueada");
    } else if (s_hostSimSeedAttempts == HOST_SIMSEED_MAX_ATTEMPTS) {
        spdlog::warn("[FIX-SIMSEED] sin completar tras {} intentos "
                     "(sembrados={} yaCorrectos={} ilegibles={} simClock={:.4f})",
                     s_hostSimSeedAttempts, seeded, alreadyOk, unreadable, simClock);
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════
//  [FIX-MEDSEED] El host (y su squad) nunca comen — hermano del FIX-SIMSEED, otro offset.
// ════════════════════════════════════════════════════════════════════════════════════════
// PROBLEMA (RE confirmado, agente RE 2026-07-12):
//   MedicalSystem::periodicUpdate (RVA 0x64DA70) deriva su dt de un timestamp propio del char:
//     MedicalSystem+0x68 == char+0x458+0x68 == char+0x4C0 (double, horas de juego).
//   El helper nativo 0x791CA0 calcula dt = SimClock - *(char+0x4C0) SIN ningún clamp, y solo
//   re-estampa char+0x4C0 al final de periodicUpdate si este llega a ejecutarse. Si char+0x4C0
//   queda con basura (memoria no inicializada del clon reclamado por el mod) o con un valor
//   "futuro" respecto al reloj de simulación, dt sale NEGATIVO → el hambre SUBE en vez de bajar
//   → el personaje nunca llega al umbral para comer.
//
// FIX (mismo mecanismo que el FIX-SIMSEED de combate, aplicado a char+0x4C0 en vez de char+0xD0):
//   sembrar char+0x4C0 = SimClock actual la primera vez que vemos/reclamamos el char del host, de
//   modo que el primer dt sea ≈0 (>=0). A partir de ahí periodicUpdate re-estampa el campo solo y
//   se auto-mantiene, igual que char+0xD0 en el combate. Write de 8 bytes bajo SEH; NO tocamos
//   estructura compartida del motor (char+0x4C0 es estado del propio char). One-shot re-armable,
//   throttled, SOLO host (mismos 2 call-sites que el FIX-SIMSEED).
//
// Guard de idempotencia: COPIA del FIX-SIMSEED (atómico one-shot re-armable + throttle 250ms +
//   máx intentos). Igual que aquel NO toca chars ya "sanos" (prev dentro de ventana), este solo
//   siembra el char cuyo prev produciría dt<0. La condición "malo" difiere del combate porque el
//   combate va atado a un gate de 12h del motor (rama cleanup) que en el subsistema médico NO
//   existe: aquí "malo" = el valor daría dt negativo (prev fuera de [0, SimClock], NaN incluido).
// EN: [FIX-MEDSEED] The host (and their squad) never eat — sibling of FIX-SIMSEED, other offset.
//     PROBLEM (RE confirmed 2026-07-12): MedicalSystem::periodicUpdate (RVA 0x64DA70) derives its dt
//       from a char-owned timestamp: MedicalSystem+0x68 == char+0x458+0x68 == char+0x4C0 (double,
//       game hours). The native helper 0x791CA0 computes dt = SimClock - *(char+0x4C0) with NO clamp
//       and only re-stamps char+0x4C0 at the end of periodicUpdate if it gets to run. If char+0x4C0
//       holds garbage (uninitialized memory of the clone claimed by the mod) or a "future" value vs
//       the simulation clock, dt goes NEGATIVE → hunger RISES instead of falling → the character
//       never reaches the threshold to eat.
//     FIX (same mechanism as the combat FIX-SIMSEED, on char+0x4C0 instead of char+0xD0): seed
//       char+0x4C0 = current SimClock the first time we see/claim the host char, so the first dt is
//       ≈0 (>=0). From then on periodicUpdate re-stamps the field and keeps itself alive. 8-byte write
//       under SEH; no shared engine structure is touched. One-shot re-armable, throttled, host ONLY.
//     Idempotency guard: COPY of FIX-SIMSEED (atomic one-shot + 250 ms throttle + max attempts). It
//       only seeds a char whose prev would give dt<0. The "bad" condition differs from combat because
//       combat is tied to a 12h engine gate that does not exist in the medical subsystem: here "bad" =
//       the value would give a negative dt (prev outside [0, SimClock], NaN included).
static std::atomic<bool> s_hostMedSeeded{false};
static int s_hostMedSeedAttempts = 0;
static auto s_lastHostMedSeedTry = std::chrono::steady_clock::now();
static constexpr int HOST_MEDSEED_MAX_ATTEMPTS = 240; // ~igual que el FIX-SIMSEED de combate

// ES: Re-arma el FIX-MEDSEED (disconnect / nueva carga).
// EN: Re-arms FIX-MEDSEED (disconnect / new load).
void ResetHostMedSeedFix() {
    s_hostMedSeeded.store(false);
    s_hostMedSeedAttempts = 0;
    s_lastHostMedSeedTry = std::chrono::steady_clock::now();
}

// Siembra char+0x4C0 (timestamp médico) = SimClock en UN char, SOLO si el valor actual daría un
// dt negativo en el helper médico 0x791CA0 (que hace dt = SimClock - *(char+0x4C0) sin clamp).
//   "malo" = !(prev >= 0.0 && prev <= simClock): cubre basura negativa, timestamp "futuro"
//   (prev > simClock → dt<0) y NaN (toda comparación false → malo). Si 0<=prev<=simClock el dt ya
//   sale >=0 (correcto) → NO tocar, igual que el FIX-SIMSEED no toca chars ya en ventana.
// Devuelve: 1=sembrado, 0=ya correcto, -1=no legible. POD, sin objetos C++ en el __try.
// EN: Seeds char+0x4C0 (medical timestamp) = SimClock on ONE char, ONLY if the current value would
//     give a negative dt in the medical helper 0x791CA0 (dt = SimClock - *(char+0x4C0), no clamp).
//     "bad" = !(prev >= 0.0 && prev <= simClock): covers negative garbage, a "future" timestamp and
//     NaN (every comparison false → bad). If 0<=prev<=simClock dt is already >=0 → do not touch.
//     Returns: 1=seeded, 0=already correct, -1=unreadable. POD, no C++ objects in __try.
static int SEH_SeedCharMedicalTimestamp(uintptr_t charPtr, double simClock, double* outPrev) {
    int result = -1;
    __try {
        double prev = 0.0;
        if (!Memory::Read(charPtr + 0x4C0, prev)) { return -1; }
        *outPrev = prev;
        bool bad = !(prev >= 0.0 && prev <= simClock);
        if (!bad) { result = 0; return 0; }          // dt ya sería >=0 → no tocar
        if (Memory::Write(charPtr + 0x4C0, simClock)) // seed = reloj → dt≈0 → el hambre baja
            result = 1;
        else
            result = -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { result = -1; }
    return result;
}

// Orquesta el seed de char+0x4C0 para los chars locales del host. Copia exacta del patrón del
// FIX-SIMSEED (throttle ~250ms, one-shot re-armable, solo host, itera GetPlayerEntities del
// jugador local). Llamado desde OnGameTick (ambas ramas), justo tras SeedHostCharLastProcessedTick.
// EN: Drives the char+0x4C0 seed for the host local chars. Exact copy of the FIX-SIMSEED pattern
//     (~250 ms throttle, re-armable one-shot, host only, iterates the local player GetPlayerEntities).
//     Called from OnGameTick (both branches), right after SeedHostCharLastProcessedTick.
static void SeedHostCharMedicalTimestampTick(Core& core) {
    if (s_hostMedSeeded.load()) return;                          // ya hecho esta partida
    if (s_hostMedSeedAttempts >= HOST_MEDSEED_MAX_ATTEMPTS) return;

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastHostMedSeedTry < std::chrono::milliseconds(250)) return;
    s_lastHostMedSeedTry = now;
    s_hostMedSeedAttempts++;

    // 1) Reloj de simulación actual como DOUBLE (mismo origen que el FIX-SIMSEED, pero sin
    //    degradar a float: char+0x4C0 es un double y necesita precisión exacta).
    // EN: 1) Current simulation clock as a DOUBLE (same source as FIX-SIMSEED, without degrading to
    //        float: char+0x4C0 is a double and needs exact precision).
    uintptr_t modBase = Memory::GetModuleBase();
    double simClock = -1.0;
    if (!SEH_ReadSimClockDouble(modBase, &simClock)) {
        // Aún no hay reloj resoluble → reintentar en el próximo throttle.
        return;
    }

    // 2) Iterar los GameObjects locales del host y sembrar el que produciría dt negativo.
    // EN: 2) Walk the host local GameObjects and seed the one that would give a negative dt.
    auto& registry = core.GetEntityRegistry();
    auto localEntities = registry.GetPlayerEntities(core.GetLocalPlayerId());
    if (localEntities.empty()) return; // el char del host aún no está registrado

    int seeded = 0, alreadyOk = 0, unreadable = 0;
    double lastPrev = 0.0;
    for (EntityID eid : localEntities) {
        void* gameObj = registry.GetGameObject(eid);
        if (!gameObj) continue;
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(gameObj);
        double prev = 0.0;
        int r = SEH_SeedCharMedicalTimestamp(charPtr, simClock, &prev);
        if (r == 1) { seeded++; lastPrev = prev; }
        else if (r == 0) alreadyOk++;
        else unreadable++;
    }

    // One-shot: hecho cuando TODOS los chars locales están correctos (sembrados o ya sanos) y al
    // menos uno se pudo leer.
    // EN: One-shot: done when ALL local chars are correct (seeded or already healthy) and at least one
    //     could be read.
    if (unreadable == 0 && (seeded + alreadyOk) > 0) {
        s_hostMedSeeded.store(true);
        spdlog::info("[FIX-MEDSEED] char+0x4C0 (timestamp medico) sembrado en chars del host — "
                     "simClock={:.4f} (sembrados={} yaCorrectos={} prevDelUltimo={:.4f}, intento {}). "
                     "Fuerza dt>=0 en MedicalSystem::periodicUpdate (0x64DA70) -> el hambre baja y "
                     "el personaje come.",
                     simClock, seeded, alreadyOk, lastPrev, s_hostMedSeedAttempts);
        if (seeded > 0)
            core.GetNativeHud().AddSystemMessage("Sistema de comida/necesidades desbloqueado");
    } else if (s_hostMedSeedAttempts == HOST_MEDSEED_MAX_ATTEMPTS) {
        spdlog::warn("[FIX-MEDSEED] sin completar tras {} intentos "
                     "(sembrados={} yaCorrectos={} ilegibles={} simClock={:.4f})",
                     s_hostMedSeedAttempts, seeded, alreadyOk, unreadable, simClock);
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════
//  [FIX-ARMSEED] "No puedo coger a cuestas a nadie con este brazo" — hermano del FIX-MEDSEED.
// ════════════════════════════════════════════════════════════════════════════════════════
// PROBLEMA (RE confirmado, agente RE 2026-07-12):
//   El gate de "cargar a cuestas" (Hook_AddOrderBackend) lee un BOOL cacheado "brazo OK"
//   (leftArmOk/rightArmOk en char+0x458+0x166 = char+0x5BE) que SOLO recalcula la función nativa
//   MedicalSystem::reassessCollapseMode (RVA 0x649320). Ese bool arranca con la basura del clon
//   reclamado por el mod y NO se refresca solo.
//
//   El fix previo (SEH_ReassessCollapse) ya se dispara tras escribir salud de extremidades por
//   red (SEH_WriteLimbHealthDirect + su gemelo en packet_handler.cpp). PERO ese camino SOLO corre
//   cuando LLEGA una escritura de salud por red. Si el brazo del char está al 100% y NUNCA se ha
//   dañado, NUNCA hay escritura de salud → el fix NUNCA se ejecuta para ese char → el bool
//   cacheado se queda mal para siempre → el gate cree que el brazo está roto aunque esté sano.
//   Es el MISMO problema de timing que el FIX-MEDSEED (esperar un evento de red que puede no
//   llegar nunca): hay que SEMBRAR el estado correcto sin depender de ese evento.
//
// FIX (mismo patrón de timing que FIX-SIMSEED/FIX-MEDSEED, aplicado al flag de colapso):
//   re-lanzar reassessCollapseMode sobre los chars locales del host de forma periódica, para que
//   el bool char+0x5BE se recalcule aunque no haya llegado ninguna escritura de salud. Reutiliza
//   SEH_ReassessCollapse (EXACTAMENTE el mismo helper que usa SEH_WriteLimbHealthDirect), que ya
//   comprueba isDead internamente y está protegido por SEH — no duplicamos lógica.
//
// Guard de idempotencia: A DIFERENCIA de FIX-SIMSEED/FIX-MEDSEED, este NO es one-shot. El bool
//   leftArmOk/rightArmOk puede volver a quedar mal en cualquier momento tras un colapso real
//   posterior, y el propio motor lo recalcula bien tras daño real; por eso re-sembrar de vez en
//   cuando es una red de seguridad inofensiva: reassessCollapseMode es IDEMPOTENTE (si el flag ya
//   está bien, no cambia nada) y barato. Por tanto: SOLO throttle largo (~2.5s), SIN flag "hecho"
//   y SIN límite de intentos. SOLO host (mismos 2 call-sites que FIX-SIMSEED/FIX-MEDSEED).
// EN: [FIX-ARMSEED] "I cannot carry anyone on my back with this arm" — sibling of FIX-MEDSEED.
//     PROBLEM (RE confirmed 2026-07-12): the "carry on back" gate (Hook_AddOrderBackend) reads a
//       cached "arm OK" BOOL (leftArmOk/rightArmOk at char+0x458+0x166 = char+0x5BE) that ONLY the
//       native MedicalSystem::reassessCollapseMode (RVA 0x649320) recomputes. That bool starts with
//       the garbage of the clone claimed by the mod and does NOT refresh by itself.
//       The previous fix (SEH_ReassessCollapse) already fires after writing limb health from the
//       network, BUT that path ONLY runs when a network health write ARRIVES. If the arm is at 100%
//       and was NEVER damaged, there is never a health write → the fix never runs → the cached bool
//       stays wrong forever. Same timing problem as FIX-MEDSEED: seed the right state without
//       waiting for that event.
//     FIX: re-run reassessCollapseMode on the host local chars periodically so char+0x5BE gets
//       recomputed even without any health write. Reuses SEH_ReassessCollapse (the same helper used
//       by SEH_WriteLimbHealthDirect), which already checks isDead and is SEH-protected.
//     Idempotency guard: UNLIKE FIX-SIMSEED/FIX-MEDSEED this is NOT one-shot: the bool can go wrong
//       again after a later real collapse, and reassessCollapseMode is IDEMPOTENT and cheap. So: only
//       a long throttle (~2.5 s), NO "done" flag and NO attempt cap. Host ONLY.

// Forward declaration del helper de recálculo de colapso. Su definición static vive más abajo en
// este mismo fichero (junto a SEH_WriteLimbHealthDirect, ~L7900). static → misma unidad de
// traducción, sin colisión de enlazado; declararlo aquí permite reutilizarlo sin duplicar código.
// clearStun: solo debe ir a true cuando la llamada sigue a una escritura de salud REAL por red
// (fleshStun rancio); el tick periódico de abajo (sin datos nuevos) debe pasar false — si no,
// borraría stun real que el motor esté acumulando en el host por combate legítimo (haciéndolo
// inmune al derribo). Hallazgo de la revisión adversarial de esta misma sesión.
// EN: Forward declaration of the collapse recompute helper. Its static definition lives further down
//     in this file (next to SEH_WriteLimbHealthDirect). static → same translation unit, no link clash.
//     clearStun: must only be true when the call follows a REAL network health write (stale
//     fleshStun); the periodic tick below (no new data) must pass false — otherwise it would erase
//     real stun the engine is accumulating on the host from legitimate combat (making it immune to
//     knockdown). Finding from the adversarial review of the same session.
static void SEH_ReassessCollapse(void* character, bool clearStun);

// Timer del throttle (NO hay flag one-shot ni contador de intentos: es idempotente, ver arriba).
// EN: Throttle timer (there is NO one-shot flag nor attempt counter: it is idempotent, see above).
static auto s_lastHostArmSeedTry = std::chrono::steady_clock::now();

// Re-lanza reassessCollapseMode sobre los chars locales del host cada ~2.5s para mantener el bool
// cacheado "brazo OK" (char+0x5BE) sincronizado con la salud real de las extremidades. Copia del
// patrón de SeedHostCharMedicalTimestampTick pero SIN one-shot ni límite de intentos (idempotente).
// Llamado desde OnGameTick (ambas ramas), justo tras SeedHostCharMedicalTimestampTick.
// EN: Re-runs reassessCollapseMode on the host local chars every ~2.5 s to keep the cached "arm OK"
//     bool (char+0x5BE) in sync with real limb health. Copy of the SeedHostCharMedicalTimestampTick
//     pattern but WITHOUT one-shot nor attempt cap (idempotent). Called from OnGameTick (both
//     branches), right after SeedHostCharMedicalTimestampTick.
static void SeedHostCharArmFlagsTick(Core& core) {
    // Throttle: cada ~2.5s basta como red de seguridad; recalcular cada tick sería derrochar.
    // EN: Throttle: every ~2.5 s is enough as a safety net; every tick would be wasteful.
    auto now = std::chrono::steady_clock::now();
    if (now - s_lastHostArmSeedTry < std::chrono::milliseconds(2500)) return;
    s_lastHostArmSeedTry = now;

    // Iterar los GameObjects locales del host. GetPlayerEntities(localPlayerId) devuelve TODAS las
    // entidades cuyo ownerPlayerId == jugador local, así que cubre tanto el char real ("Zero") como
    // el/los fantasmas ("Player 1") — el mismo conjunto que ya recorren FIX-SIMSEED y FIX-MEDSEED.
    // EN: Walk the host local GameObjects. GetPlayerEntities(localPlayerId) returns ALL entities whose
    //     ownerPlayerId == local player, so it covers both the real char and the ghost(s) ("Player 1") —
    //     the same set FIX-SIMSEED and FIX-MEDSEED walk.
    auto& registry = core.GetEntityRegistry();
    auto localEntities = registry.GetPlayerEntities(core.GetLocalPlayerId());
    if (localEntities.empty()) return; // el char del host aún no está registrado

    for (EntityID eid : localEntities) {
        void* gameObj = registry.GetGameObject(eid);
        if (!gameObj) continue;
        // Reutiliza el MISMO helper que SEH_WriteLimbHealthDirect: comprueba isDead y está bajo SEH.
        // clearStun=false: este tick NO sigue a ninguna escritura de salud por red — no hay stun
        // "rancio" que limpiar, y borrarlo aquí anularía stun real de combate legítimo del host.
        // EN: Reuses the SAME helper as SEH_WriteLimbHealthDirect (checks isDead, runs under SEH).
        //     clearStun=false: this tick does not follow any network health write — there is no stale stun
        //     to clear, and clearing it would cancel legitimate host combat stun.
        SEH_ReassessCollapse(gameObj, /*clearStun=*/false);
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════
//  [FIX-CONTROL] CAUSA 2 del combate congelado — vincular el char del HOST como "controlado
//  por el jugador" vía SetControlledChar (RVA 0x802520).            [Fase 4 — 2026-06-19]
// ════════════════════════════════════════════════════════════════════════════════════════
// PROBLEMA (RE de bytes confirmado, Steam 1.0.68, agente RE 2026-06-19):
//   El char del host CAMINA pero NO ataca/cura/levanta porque su "think" pesado (vtbl+0x1D8 =
//   0x5CE020: atacar/curar/Task_GetUp/auto-defensa/auto-medic) NO corre de forma continua. El
//   "think" vive tras el gate de la rama viva 0x5CD1C0:
//     0x5CD1DA  mov  rax,[rdi]           ; rax = vtable del char (rdi = char)
//     0x5CD1E0  call [rax+0x58]          ; getController(char) -> devuelve char.faction (char+0x10)
//     0x5CD1E3  cmp  [rax+0x250],rsi     ; ¿faction+0x250 != 0?
//     0x5CD1EA  jne  0x5CD1FD            ; SÍ -> SALTA el umbral 0.75 -> PIENSA SIEMPRE
//     0x5CD1EC  movss xmm0,[0.75]; comiss xmm0,[char+0xD8]; jbe salir  ; si faction+0x250==0,
//                                          el think solo corre intermitente (lento) -> combate roto.
//
//   CLAVE (corrección de RE sobre la hipótesis previa): el gate NO lee char+0x250 (el AI tick
//   0x5CCD90 lo BORRA a 0 en cada entrada, 0x5CCDD1 `mov byte[char+0x250],0`). El gate lee
//   getController(char)+0x250, y getController = vtbl+0x58 = devuelve char.faction (char+0x10).
//   Por tanto el campo que exime del umbral es FACTION+0x250, NO char+0x250.
//
//   Quién escribe faction+0x250: SOLO SetControlledChar (RVA 0x802520). Verificado por bytes:
//     SetControlledChar(PlayerInterface* this /*rcx*/, Faction* newFaction /*rdx*/):
//       0x80254E  mov rax,[rcx+0x2A0]          ; oldFaction
//       0x80255C  if old!=0: mov [old+0x250],0 ; limpia el back-ptr del controlador anterior
//       0x802563  mov [rcx+0x2A0],rdx          ; PI+0x2A0 = newFaction
//       0x80256A  mov [rdx+0x250],rcx          ; *** newFaction+0x250 = PlayerInterface ***  <-- LO QUE QUEREMOS
//       0x80267A  ... PI+0x2A8 = newFaction.members[count-1]  ; deriva el char controlado
//   El mod NUNCA llamaba a SetControlledChar → faction+0x250 == 0 → el host nunca queda exento.
//
//   ⚠ CORRECCIÓN al plan original: el argumento de SetControlledChar es la FACTION del jugador
//   (PI+0x2A0), NO el char. Pasamos GetPlayerFactionDirect() (= *(PI+0x2A0) = 'Nameless'/host).
//
// FIX (preferido): llamar UNA vez SetControlledChar(PI, playerFaction). Como PI+0x2A0 YA es
//   playerFaction, esto es idempotente y de bajo riesgo: re-vincula la MISMA facción y escribe
//   faction+0x250 = PI. El char del host queda EXENTO del umbral → su think corre siempre →
//   procesa órdenes de combate + auto-defensa/medic como en single-player.
//
// SEGURIDAD/RIESGO (evaluado): SetControlledChar cambia el "controlledChar" del jugador
//   (PI+0x2A8 = members[count-1]). Para el host single-player el char que el jugador controla YA
//   es ese, así que re-vincular la misma facción no roba el control a nadie. El único efecto
//   colateral teórico: si el motor tuviera otro controlledChar activo, lo re-derivaría a
//   members[count-1]; pero como pasamos la facción que YA está en PI+0x2A0, el resultado es el
//   mismo char. Llamada bajo SEH, guard atómico idempotente (una vez por partida). NUNCA
//   escribimos faction+0x250 ni char+0x250 a pelo (son punteros; usamos el setter del motor).
//
// FIX 2 ALTERNATIVO (menos invasivo, detrás de toggle — ver kUseSetControlledChar abajo):
//   forzar char+0xDC = 1 cada frame antes de que corra el AI tick, replicando el writer nativo
//   0x645B7D (`mov byte[char+0xDC],1`). char+0xDC es el flag "needs think" del gate 0x5CD1CD
//   (cmp [char+0xDC],0; je salir). Ponerlo a 1 hace que el char ENTRE al bloque del think; aún
//   así, si faction+0x250==0 el think queda bajo el umbral 0.75 (intermitente). Por eso el FIX 2
//   es un PALIATIVO (mejora la frecuencia del think) y el FIX 1 (SetControlledChar) es la cura
//   real. Se deja preparado para poder elegir en runtime si el FIX 1 diera efecto colateral.
// EN: [FIX-CONTROL] CAUSE 2 of the frozen combat — bind the HOST char as "player controlled" via
//     SetControlledChar (RVA 0x802520). [Phase 4 — 2026-06-19]
//     PROBLEM (byte-level RE, Steam 1.0.68): the host char WALKS but does NOT attack/heal/get up
//       because its heavy "think" (vtbl+0x1D8 = 0x5CE020: attack/heal/Task_GetUp/self-defense/
//       auto-medic) does not run continuously. The think sits behind the live-branch gate 0x5CD1C0:
//       getController(char) [vtbl+0x58] returns char.faction (char+0x10); if faction+0x250 != 0 the
//       0.75 threshold is SKIPPED and the char ALWAYS thinks; otherwise it only thinks
//       intermittently (compares 0.75 against char+0xD8) → broken combat.
//       KEY: the gate does NOT read char+0x250 (the AI tick 0x5CCD90 clears it every entry); it reads
//       FACTION+0x250.
//       Only SetControlledChar(PlayerInterface* this, Faction* newFaction) writes faction+0x250: it
//       clears the old faction back-pointer, sets PI+0x2A0 = newFaction, writes
//       newFaction+0x250 = PlayerInterface (WHAT WE WANT) and derives PI+0x2A8 =
//       newFaction.members[count-1]. The mod NEVER called it → faction+0x250 == 0.
//       CORRECTION to the original plan: the argument is the player FACTION (PI+0x2A0), NOT the char.
//     FIX (preferred): call SetControlledChar(PI, playerFaction) ONCE. Since PI+0x2A0 already IS
//       playerFaction this is idempotent and low risk: it re-binds the SAME faction and writes
//       faction+0x250 = PI → the host char is EXEMPT from the threshold → its think always runs.
//     SAFETY: it changes the player "controlledChar" (PI+0x2A8 = members[count-1]); passing the
//       faction already in PI+0x2A0 yields the same char. Under SEH, atomic idempotent guard (once per
//       game). faction+0x250 / char+0x250 are NEVER written raw (they are pointers; the engine setter
//       is used).
//     ALTERNATIVE FIX 2 (less invasive, behind toggle kUseSetControlledChar): force char+0xDC = 1 each
//       frame (the "needs think" flag of gate 0x5CD1CD), replicating the native writer 0x645B7D. It
//       only improves think frequency (still under the 0.75 threshold) → a PALLIATIVE; FIX 1 is the
//       real cure.

// Firma nativa de SetControlledChar (RVA 0x802520). Prólogo `40 57 48 83 EC 60` (push rdi;
// sub rsp,0x60) → NO es `mov rax,rsp`, no requiere el fix MovRaxRsp (además NO la hookeamos,
// la LLAMAMOS). __fastcall(this=PlayerInterface* en rcx, newFaction=Faction* en rdx).
// EN: Native signature of SetControlledChar (RVA 0x802520). Prologue `40 57 48 83 EC 60` (push rdi;
//     sub rsp,0x60) → NOT `mov rax,rsp`, no MovRaxRsp fix needed (and we do NOT hook it, we CALL it).
//     __fastcall(this=PlayerInterface* in rcx, newFaction=Faction* in rdx).
using SetControlledCharFn = void(__fastcall*)(uintptr_t playerInterface, uintptr_t newFaction);

// Toggle de runtime: true = FIX 1 (SetControlledChar, la cura real). false = FIX 2 (forzar
// char+0xDC=1 cada frame, paliativo menos invasivo). DEFAULT true (FIX 1 es seguro e idempotente).
// EN: Runtime toggle: true = FIX 1 (SetControlledChar, the real cure). false = FIX 2 (force
//     char+0xDC=1 every frame, less invasive palliative). DEFAULT true (FIX 1 is safe and idempotent).
static constexpr bool kUseSetControlledChar = true;

// Estado one-shot del FIX 1 (re-armable en disconnect / nueva carga vía ResetHostControlledCharFix).
// EN: FIX 1 one-shot state (re-armable on disconnect / new load via ResetHostControlledCharFix).
static std::atomic<bool> s_hostControlledSet{false};
static int  s_hostControlledAttempts = 0;
static auto s_lastHostControlledTry  = std::chrono::steady_clock::now();
static constexpr int HOST_CONTROLLED_MAX_ATTEMPTS = 240; // ~varios segundos de reintentos

// ES: Re-arma el FIX-CONTROL (disconnect / nueva carga).
// EN: Re-arms FIX-CONTROL (disconnect / new load).
void ResetHostControlledCharFix() {
    s_hostControlledSet.store(false);
    s_hostControlledAttempts = 0;
    s_lastHostControlledTry = std::chrono::steady_clock::now();
}

// Snapshot POD para el DIAG [FIX-CONTROL] (sin objetos C++ → vive dentro del __try, evita C2712).
// EN: POD snapshot for the [FIX-CONTROL] DIAG (no C++ objects → lives inside __try, avoids C2712).
//     Fields: PI = *(GW+0x580) (PlayerInterface); PI+0x2A0 = player faction; PI+0x2A8 = controlled
//     char; hostChar = data[0] of PI+0x2B0; hostChar+0x10 = its faction; faction+0x250 before/after
//     the fix (the gate field; 0 = not exempt); char+0x250 (cleared by the AI tick, informative).
struct ControlDiagSnapshot {
    int       resolved        = 0;     // 1 si se resolvió PI + faction
    uintptr_t playerIface     = 0;     // PI = *(GW+0x580)
    uintptr_t pi2A0_faction   = 0;     // PI+0x2A0 (facción/participant del jugador)
    uintptr_t pi2A8_ctrlChar  = 0;     // PI+0x2A8 (char controlado que deriva SetControlledChar)
    uintptr_t hostChar        = 0;     // GetPlayerPrimaryCharacterDirect (data[0] de PI+0x2B0) — lo que lee el mod
    uintptr_t hostCharFaction = 0;     // hostChar+0x10 (debería == pi2A0_faction)
    uintptr_t faction250_pre  = 0;     // faction+0x250 ANTES del fix (el campo del gate; 0 = no exento)
    uintptr_t faction250_post = 0;     // faction+0x250 DESPUÉS del fix (debería = PI != 0)
    uintptr_t char250         = 0;     // char+0x250 (lo que el AI tick borra cada frame; informativo)
    int       called          = 0;     // 1 si se llamó a SetControlledChar sin excepción
};

// Lee la cadena PI/faction/char y faction+0x250 ANTES de llamar al setter. SEH, POD.
// EN: Reads the PI/faction/char chain and faction+0x250 BEFORE calling the setter. SEH, POD.
static void SEH_ReadControlPre(uintptr_t gwObj, uintptr_t modBase, size_t modSize,
                               uintptr_t hostChar, ControlDiagSnapshot* out) {
    if (gwObj == 0) return;
    auto isHeap = [modBase, modSize](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        if (v >= modBase && v < modBase + modSize) return false;
        return true;
    };
    __try {
        uintptr_t pi = 0;
        if (!Memory::Read(gwObj + 0x580, pi) || !isHeap(pi)) return;
        out->playerIface = pi;
        Memory::Read(pi + 0x2A0, out->pi2A0_faction);
        Memory::Read(pi + 0x2A8, out->pi2A8_ctrlChar);
        out->hostChar = hostChar;
        if (isHeap(hostChar)) Memory::Read(hostChar + 0x10, out->hostCharFaction);
        if (isHeap(out->pi2A0_faction))
            Memory::Read(out->pi2A0_faction + 0x250, out->faction250_pre);
        if (isHeap(hostChar)) Memory::Read(hostChar + 0x250, out->char250);
        out->resolved = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Llama a SetControlledChar(PI, faction) bajo SEH y relee faction+0x250 después. POD.
// EN: Calls SetControlledChar(PI, faction) under SEH and re-reads faction+0x250 afterwards. POD.
static void SEH_CallSetControlled(SetControlledCharFn fn, uintptr_t pi, uintptr_t faction,
                                  ControlDiagSnapshot* out) {
    if (fn == nullptr || pi == 0 || faction == 0) return;
    __try {
        fn(pi, faction);                 // escribe faction+0x250 = PI (gate exento)
        out->called = 1;
        Memory::Read(faction + 0x250, out->faction250_post);
    } __except (EXCEPTION_EXECUTE_HANDLER) { out->called = -1; }
}

// FIX 2 (paliativo): fuerza char+0xDC = 1 (flag "needs think" del gate 0x5CD1CD). SEH, POD.
// Devuelve 1 si escribió, 0 si ya estaba a 1, -1 si no legible/escribible.
// EN: FIX 2 (palliative): forces char+0xDC = 1 ("needs think" flag of gate 0x5CD1CD). SEH, POD.
//     Returns 1 if written, 0 if it was already 1, -1 if unreadable/unwritable.
static int SEH_ForceNeedsThink(uintptr_t charPtr) {
    int r = -1;
    __try {
        uint8_t cur = 0;
        if (!Memory::Read(charPtr + 0xDC, cur)) return -1;
        if (cur != 0) return 0;
        r = Memory::Write(charPtr + 0xDC, static_cast<uint8_t>(1)) ? 1 : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    return r;
}

// Orquesta el FIX-CONTROL. Mismo patrón que FixHostCharacterFactionTick / Seed: throttle 250ms,
// one-shot re-armable, SOLO host, en el HILO DE LÓGICA (OnGameTick). Idempotente.
// EN: Drives FIX-CONTROL. Same pattern as FixHostCharacterFactionTick / Seed: 250 ms throttle,
//     re-armable one-shot, host ONLY, on the LOGIC THREAD (OnGameTick). Idempotent.
static void SetHostControlledCharTick(Core& core) {
    // ── FIX 2 (paliativo): si el toggle elige el alternativo, forzar char+0xDC=1 cada frame ──
    // NO es one-shot: el flag se consume cada vez que el char piensa, así que se re-fuerza.
    // EN: ── FIX 2 (palliative): if the toggle picks the alternative, force char+0xDC=1 every frame ──
    //     NOT one-shot: the flag is consumed each time the char thinks, so it is re-forced.
    if (!kUseSetControlledChar) {
        auto& registry = core.GetEntityRegistry();
        auto localEntities = registry.GetPlayerEntities(core.GetLocalPlayerId());
        int forced = 0;
        for (EntityID eid : localEntities) {
            void* gameObj = registry.GetGameObject(eid);
            if (!gameObj) continue;
            if (SEH_ForceNeedsThink(reinterpret_cast<uintptr_t>(gameObj)) == 1) forced++;
        }
        static int s_fix2Log = 0;
        if (forced > 0 && ++s_fix2Log <= 5) {
            spdlog::info("[FIX-CONTROL] FIX 2 (paliativo) activo: char+0xDC forzado a 1 en {} "
                         "char(s) del host (flag 'needs think' del gate 0x5CD1CD).", forced);
        }
        return;
    }

    // ── FIX 1 (preferido): SetControlledChar(PI, playerFaction). One-shot idempotente. ──
    // EN: ── FIX 1 (preferred): SetControlledChar(PI, playerFaction). Idempotent one-shot. ──
    if (s_hostControlledSet.load()) return;
    if (s_hostControlledAttempts >= HOST_CONTROLLED_MAX_ATTEMPTS) return;

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastHostControlledTry < std::chrono::milliseconds(250)) return;
    s_lastHostControlledTry = now;
    s_hostControlledAttempts++;

    // GameWorld + módulo.
    // EN: GameWorld + module (image size read from the PE header, 64 MB fallback).
    uintptr_t modBase = Memory::GetModuleBase();
    if (modBase == 0) return;
    size_t modSize = 0x4000000;
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) modSize = nt->OptionalHeader.SizeOfImage;
        }
    }
    // GameWorld resuelto (mismo método que [FIX-HOSTILITY] / la rama de pausa de OnGameTick).
    // EN: Resolved GameWorld (same method as [FIX-HOSTILITY] / the OnGameTick pause branch).
    game::GameWorldAccessor world(game::GetResolvedGameWorld());
    uintptr_t gwObj = world.IsValid() ? world.GetWorldObject() : 0;
    if (gwObj == 0) return;

    // PI = *(GW+0x580); playerFaction = GetPlayerFactionDirect() (= *(PI+0x2A0), 'Nameless'/host).
    // EN: PI = *(GW+0x580); playerFaction = GetPlayerFactionDirect() (= *(PI+0x2A0), 'Nameless'/host).
    uintptr_t playerFaction = game::GetPlayerFactionDirect();
    if (playerFaction == 0) return;  // facción aún no resoluble → reintentar

    // hostChar (informativo para el DIAG): data[0] de PI+0x2B0 (lo que lee el mod).
    // EN: hostChar (informative for the DIAG): data[0] of PI+0x2B0 (what the mod reads).
    uintptr_t hostChar = game::GetPlayerPrimaryCharacterDirect();

    // 1) Snapshot PRE (faction+0x250 antes, reconciliación PI+0x2A0 vs +0x2A8).
    // EN: 1) PRE snapshot (faction+0x250 before, PI+0x2A0 vs +0x2A8 reconciliation).
    ControlDiagSnapshot ds{};
    SEH_ReadControlPre(gwObj, modBase, modSize, hostChar, &ds);
    if (!ds.resolved || ds.playerIface == 0) return;  // PI no resoluble → reintentar

    // 2) Si faction+0x250 YA != 0, el host ya está exento (otra partida / ya aplicado): no repetir.
    // EN: 2) If faction+0x250 is ALREADY != 0 the host is already exempt (other game / already applied).
    if (ds.faction250_pre != 0) {
        s_hostControlledSet.store(true);
        spdlog::info("[FIX-CONTROL] faction+0x250 ya era 0x{:X} (!=0) → el host YA estaba exento "
                     "del umbral (PI=0x{:X} faction=0x{:X}). No se re-aplica.",
                     ds.faction250_pre, ds.playerIface, playerFaction);
        return;
    }

    // 3) Llamar al setter nativo SetControlledChar(PI, playerFaction). Escribe faction+0x250=PI.
    // EN: 3) Call the native setter SetControlledChar(PI, playerFaction) at modBase+0x802520. Writes
    //        faction+0x250=PI.
    SetControlledCharFn setCtl = reinterpret_cast<SetControlledCharFn>(modBase + 0x802520);
    SEH_CallSetControlled(setCtl, ds.playerIface, playerFaction, &ds);

    // 4) Verificar y loguear (DIAG-PRIMARY: ¿faction+0x250 pasó de 0 a !=0? ¿PI+0x2A0 vs +0x2A8?).
    // EN: 4) Verify and log (did faction+0x250 go from 0 to !=0? PI+0x2A0 vs +0x2A8?).
    if (ds.called == 1 && ds.faction250_post != 0) {
        s_hostControlledSet.store(true);
        spdlog::info(
            "[FIX-CONTROL] APLICADO ✓ SetControlledChar(PI=0x{:X}, faction=0x{:X}) — "
            "faction+0x250: 0x{:X} -> 0x{:X} (debe == PI). "
            "Reconciliación: PI+0x2A0(faction)=0x{:X} PI+0x2A8(ctrlChar)=0x{:X} "
            "hostChar(mod,data[0])=0x{:X} hostChar+0x10(faction)=0x{:X} char+0x250=0x{:X} "
            "(intento {}). El gate 0x5CD1E3 ahora exime al host del umbral 0.75 -> think continuo "
            "-> combate/cura desbloqueados.",
            ds.playerIface, playerFaction, ds.faction250_pre, ds.faction250_post,
            ds.pi2A0_faction, ds.pi2A8_ctrlChar, ds.hostChar, ds.hostCharFaction,
            ds.char250, s_hostControlledAttempts);
        core.GetNativeHud().AddSystemMessage("Control de combate del jugador vinculado");
    } else if (ds.called == -1) {
        // Excepción dentro de la llamada nativa → no insistir agresivamente, pero re-intentar.
        if (s_hostControlledAttempts <= 5 || s_hostControlledAttempts % 60 == 0) {
            spdlog::warn("[FIX-CONTROL] SetControlledChar lanzó excepción (PI=0x{:X} faction=0x{:X}) "
                         "— reintento {}.", ds.playerIface, playerFaction, s_hostControlledAttempts);
        }
    } else if (s_hostControlledAttempts == HOST_CONTROLLED_MAX_ATTEMPTS) {
        spdlog::warn("[FIX-CONTROL] sin completar tras {} intentos (called={} faction250_post=0x{:X} "
                     "PI=0x{:X} faction=0x{:X}). Revisar si SetControlledChar 0x802520 es correcto.",
                     s_hostControlledAttempts, ds.called, ds.faction250_post,
                     ds.playerIface, playerFaction);
    }
}

// ══════════════════════════════════════════════════════════════════════════════════
// [FIX-AI-NULL] — El char del host nace con char+0x650 (AI*) == NULL.
// ══════════════════════════════════════════════════════════════════════════════════
// HALLAZGO RUNTIME (log 2 jugadores, [DIAG-COMBATSTRUCT]): el char primario del host
// tiene char+0x650 (AI) == NULL. CAMINA porque char+0x640 (CharMovement) sí existe, pero
// NO piensa / NO combate / NO es objetivo: sin el subsistema de IA el AI tick no encuentra
// cola de tareas ni estado de combate. En sesiones PRE-Nameless el AI era válido
// (p.ej. 0xA2ED9C60); post-Nameless aparece NULL.
//
// ─────────────────────────────────────────────────────────────────────────────────────
// RE DE BYTES — ¿quién crea el AI y lo escribe en char+0x650? (Steam 1.0.68, ke_re.py)
// ─────────────────────────────────────────────────────────────────────────────────────
// Cruce con KenshiLib (firmas exactas) + delta de versión +0x780 (ventana de combate,
// audit-12) + verificación directa de bytes del binario:
//
//   • Character::createComponents(GameDataCopyStandalone* appearance)
//       KenshiLib RVA 0x62A7D0  → Steam 0x62AF50  (+0x780). VERIFICADO:
//       - prólogo 'mov rax,rsp' (48 8B C4) en 0x62AF50.
//       - lee rdx = appearance (mov r12,rdx en +0x2B).
//       - en 0x62AF84: call [vtable+0x228] (init/createPhysical — código pesado del motor).
//       - en 0x62AFB8: *** mov [rsi+0x650], rax ***  ← ÚNICO sitio del binario (de 5 totales
//         que escriben [reg+0x650]) que aloca el AI y lo guarda en char+0x650. ESTA es la
//         función que "crea el AI ausente".
//       - acto seguido aloca char+0x648 (CharBody, 0x3B8 bytes) y char+0x640 (CharMovement).
//       ⛔ NO es idempotente: SOBREESCRIBE char+0x650/+0x648/+0x640 INCONDICIONALMENTE (sin
//         test previo). Si el host YA tiene CharBody/CharMovement (y los tiene: camina),
//         llamar createComponents los RE-ALOCA → fuga de los antiguos + estado a medias →
//         corrupción/crash casi seguros. Además EXIGE un GameDataCopyStandalone* válido en rdx.
//
//   • Character::giveBirth(appearance, pos, rot, GameSaveState*, ActivePlatoon*, Faction*)
//       vtable Character +0x378 → thunk 0xA687 → 0x62B210 (KenshiLib 0x62AA90 +0x780).
//       VERIFICADO (vtable+0x378 qword = 0x14000A687, jmp → 0x62B210). Es el constructor de
//       alto nivel: 6 args complejos. Llamarla sobre un char YA nacido = doble nacimiento =
//       catástrofe (re-corre toda la cadena create→createComponents).
//
//   • Character::setupAI()  (KenshiLib 0x6213F0; el proyecto la conoce como setActivePlatoon
//       0x6213F0 — misma región, RE previo del FIX-PLATOON). VERIFICADO: LEE [char+0x650]
//       repetidamente (mov rcx,[rdi+650h] en 0x621BDB/0x621C47/0x621CDD) y hace call [rax+58h]
//       sobre él. ASUME que el AI YA EXISTE: lo configura, NO lo crea. Si char+0x650==NULL →
//       deref de NULL → CRASH. NO sirve para arreglar AI==NULL.
//
//   • AI::create / AICreate 0x622110: this=rcx es el PROPIO objeto AI (escribe AI+0x300..+0x318,
//       AI+0x8), NO el Character. Es el ctor interno del AI; no escribe char+0x650 por sí mismo
//       (lo invoca createComponents tras alocar el bloque). Llamarla suelta no enlaza nada.
//
// ─────────────────────────────────────────────────────────────────────────────────────
// VEREDICTO DE SEGURIDAD — POR QUÉ EL FLAG QUEDA EN false (NO se invoca a ciegas)
// ─────────────────────────────────────────────────────────────────────────────────────
// NINGUNA de las funciones que tocan char+0x650 es segura de invocar sobre un char ya
// existente al que solo le falta el AI:
//   - setupAI/setActivePlatoon LEEN char+0x650 → crashean si es NULL.
//   - createComponents/giveBirth lo CREAN, pero RE-ALOCAN el resto de subsistemas (body,
//     movement) sin guard de idempotencia y EXIGEN un GameDataCopyStandalone* (appearance)
//     + (giveBirth) pos/rot/state/platoon/faction. Sobre un char ya nacido = corrupción/crash.
// El prompt es explícito: "mejor un DLL que no crashea que uno que sí". Como NO se puede
// confirmar una firma SEGURA (idempotente, sin args externos) que cree solo el AI, este fix
// queda DOCUMENTADO y DESACTIVADO (kFixHostAiNull=false). El helper SEH_DiagHostAiNull SOLO
// LEE/loguea el estado de char+0x650 (antes/después conceptual) para confirmar el síntoma en
// runtime sin tocar memoria. La vía de invocación queda escrita pero tras el guard del flag.
//
// ─────────────────────────────────────────────────────────────────────────────────────
// ¿LO CAUSÓ NAMELESS? — el ángulo del "char equivocado" (punto 2 del encargo)
// ─────────────────────────────────────────────────────────────────────────────────────
// GetPlayerPrimaryCharacterDirect() reclama data[0] de la lektor PlayerInterface+0x2B0
// (game_character.cpp:913): el PRIMER Character* de la lista del jugador. Dos observaciones:
//   (a) El FIX-COMBATCLASS (RE previo, líneas ~2825) documenta que el char del host tiene
//       AI(+0x650) PRESENTE y solo CombatClass(+0x8) NULL. El nuevo log dice AI==NULL. Esa
//       CONTRADICCIÓN sugiere que NO es el mismo char: con Nameless el orden/contenido de la
//       lista cambió (antes 'Zero/Pepo' + 'Player 1'; ahora otro), y data[0] puede apuntar a
//       un char insertado a medio nacer (AI aún no creado) en vez del char realmente jugable.
//   (b) Si el problema es el CHAR EQUIVOCADO, el arreglo correcto NO es fabricar el AI a mano
//       (peligroso) sino RECLAMAR EL CHAR CORRECTO: el que tiene char+0x650 con vtable de AI
//       válida (== modBase+0x16FA3E8). El helper de abajo vuelca, para los primeros N chars de
//       la lista, cuál tiene AI válido — para decidir en runtime si basta cambiar el índice
//       reclamado (vía data[i] con AI!=NULL) en lugar de crear el subsistema.
//
// SEGURIDAD/THREAD: igual que FIX-COMBATCLASS/FIX-PLATOON — SOLO host, HILO DE LÓGICA
// (OnGameTick), nunca desde el callback de red. Todo bajo SEH en helper POD (sin objetos C++
// en el __try → evita C2712). One-shot re-armable. Throttle 250ms.
// EN: [FIX-AI-NULL] — The host char is born with char+0x650 (AI*) == NULL.
//     RUNTIME FINDING (2-player log, [DIAG-COMBATSTRUCT]): the host primary char has char+0x650 (AI)
//       == NULL. It WALKS because char+0x640 (CharMovement) exists, but it does NOT think / fight /
//       get targeted: without the AI subsystem the AI tick finds no task queue nor combat state. In
//       PRE-Nameless sessions the AI was valid; post-Nameless it appears NULL.
//     BYTE RE — who creates the AI and writes char+0x650? (Steam 1.0.68, KenshiLib + version delta
//       +0x780):
//       • Character::createComponents(appearance): KenshiLib 0x62A7D0 → Steam 0x62AF50. The ONLY
//         place in the binary that allocates the AI and stores it at char+0x650; it then allocates
//         char+0x648 (CharBody) and char+0x640 (CharMovement). NOT idempotent: it overwrites all
//         three unconditionally → leaks + half state → near-certain corruption/crash on a char that
//         already has them. Also requires a valid GameDataCopyStandalone* (appearance).
//       • Character::giveBirth(...) (vtable +0x378 → 0x62B210): high-level constructor with 6 complex
//         args; calling it on an already-born char = double birth = catastrophe.
//       • Character::setupAI() (KenshiLib 0x6213F0 → Steam 0x621B70): READS char+0x650 and calls
//         into it; it ASSUMES the AI exists. With NULL → crash. Does not fix AI==NULL.
//       • AI::create / AICreate 0x622110: this = the AI object itself, internal ctor; it does not link
//         anything to the Character by itself.
//     SAFETY VERDICT — why the flag stays false: NONE of the functions that touch char+0x650 is safe
//       on an existing char that only lacks the AI. "Better a DLL that does not crash than one that
//       does." The fix stays DOCUMENTED and DISABLED (kFixHostAiNull=false). SEH_DiagHostAiNull only
//       READS/logs char+0x650 to confirm the symptom at runtime without touching memory.
//     WAS IT NAMELESS? — the "wrong char" angle: GetPlayerPrimaryCharacterDirect() claims data[0] of
//       the PlayerInterface+0x2B0 lektor. (a) FIX-COMBATCLASS documents the host char with a PRESENT
//       AI and only CombatClass NULL; the new log says AI==NULL → it is probably NOT the same char
//       (with Nameless the list order/content changed and data[0] may be a half-born char).
//       (b) If so, the right fix is to CLAIM THE RIGHT CHAR (the one whose char+0x650 has a valid AI
//       vtable == modBase+0x16FA3E8), not to build the AI by hand. The helper below reports which
//       list entry has a valid AI.
//     SAFETY/THREAD: host ONLY, LOGIC THREAD (OnGameTick), never from the network callback. All under
//       SEH in a POD helper. Re-armable one-shot. 250 ms throttle.
//     (The "lines ~2825" reference above is approximate and out of date.)
// ES: (La referencia "líneas ~2825" de arriba es aproximada y está desfasada. Nota: el FIX-PLATOON
//     llama "setActivePlatoon 0x6213F0" a una RVA que aquí se identifica como setupAI de KenshiLib
//     (0x6213F0 → Steam 0x621B70); sin verificar cuál de las dos lecturas es la correcta.)
// EN: Note: FIX-PLATOON calls "setActivePlatoon 0x6213F0" an RVA that is identified here as the
//     KenshiLib setupAI (0x6213F0 → Steam 0x621B70); not verified which reading is correct.

// Toggle maestro del FIX-AI-NULL. DEFAULT **false** (no se invoca ninguna creación de AI a
// ciegas: ver VEREDICTO DE SEGURIDAD arriba). Con el flag en false, FixHostAiNullTick SOLO
// diagnostica (lee/loguea char+0x650 y busca un char alternativo con AI válido); NO escribe
// memoria del juego. Subir a true ÚNICAMENTE si se confirma una firma segura de creación de AI.
// EN: Master toggle of FIX-AI-NULL. DEFAULT **false** (no blind AI creation is invoked: see the SAFETY
//     VERDICT above). With the flag false, FixHostAiNullTick ONLY diagnoses (reads/logs char+0x650 and
//     looks for an alternative char with a valid AI); it does NOT write game memory. Raise to true
//     ONLY if a safe AI-creation signature is confirmed.
static constexpr bool kFixHostAiNull = false;

// Firmas RE-verificadas (sin usar mientras el flag esté en false; documentadas para activación).
// EN: RE-verified signatures (unused while the flag is false; documented for activation).
//   createComponents 0x62AF50: bool(__fastcall)(Character* this, GameDataCopyStandalone* appearance)
using CreateComponentsFn = bool(__fastcall*)(void* character, void* appearance);
//   setupAI 0x621B70 (lee char+0x650): void(__fastcall)(Character* this)  ← NO arregla NULL
using SetupAiFn = void(__fastcall*)(void* character);

// Estado one-shot del FIX-AI-NULL (re-armable en disconnect / nueva carga).
// EN: FIX-AI-NULL one-shot state (re-armable on disconnect / new load).
static std::atomic<bool> s_aiNullFixDone{false};
static int  s_aiNullAttempts = 0;
static auto s_lastAiNullTry  = std::chrono::steady_clock::now();
static constexpr int AINULL_MAX_ATTEMPTS = 240; // ~varios segundos (≈throttle 250ms), igual que COMBATCLASS

// Re-arma el FIX-AI-NULL para una partida/conexión nueva (mismo patrón que FIX-COMBATCLASS).
// EN: Re-arms FIX-AI-NULL for a new game/connection (same pattern as FIX-COMBATCLASS). Not declared
//     in core.h (unlike the other Reset* functions).
// ES: No está declarada en core.h (a diferencia del resto de funciones Reset*).
void ResetHostAiNullFix() {
    s_aiNullFixDone.store(false);
    s_aiNullAttempts = 0;
    s_lastAiNullTry = std::chrono::steady_clock::now();
}

// Snapshot POD del FIX-AI-NULL (vive dentro del __try; sin objetos C++ → evita C2712).
// EN: FIX-AI-NULL POD snapshot (lives inside __try; no C++ objects → avoids C2712). Holds the host
//     char, its AI before/after (and whether its vtable matches the known AI vtable), movement/body,
//     whether it already had a valid AI, whether createComponents was called, and the "wrong char"
//     angle: first list entry with a valid AI (index, char, AI) plus the player list size.
struct AiNullFixSnapshot {
    uintptr_t hostChar       = 0;  // data[0] de la lista del jugador (lo que reclama el mod)
    uintptr_t aiPre          = 0;  // char+0x650 ANTES — 0 = AI ausente (síntoma)
    int       aiPreVtblOk     = 0; // 1 si aiPre válido y vtable == kAIVtableRVA(abs)
    uintptr_t aiPost         = 0;  // char+0x650 DESPUÉS (igual a aiPre mientras el flag esté en false)
    int       aiPostVtblOk    = 0; // 1 si aiPost válido y vtable correcta
    uintptr_t movement       = 0;  // char+0x640 (debe existir: el host camina)
    uintptr_t body           = 0;  // char+0x648
    int       alreadyHad     = 0;  // 1 si char+0x650 ya tenía un AI válido (idempotencia: nada que hacer)
    int       called         = 0;  // 0 no se llamó (flag off) · 1 llamado · -1 excepción en la llamada
    // ── Ángulo "char equivocado": primer char de la lista con AI VÁLIDO (índice alternativo) ──
    int       altIndex       = -1; // índice i (data[i]) con AI válido != hostChar ; -1 si ninguno
    uintptr_t altChar        = 0;  // ese Character*
    uintptr_t altAi          = 0;  // su char+0x650 (AI válido)
    int       listSize       = 0;  // tamaño de la lista del jugador (PlayerInterface+0x2B0+0x08)
};

// Lee el estado del AI del host y (sin activar nada mientras kFixHostAiNull=false) busca un char
// alternativo con AI válido en la lista del jugador. SOLO LECTURA salvo que el flag se active.
//   chr            = host char (data[0])
//   aiVtblAbs      = modBase + kAIVtableRVA (vtable conocida del AI para validar)
//   listData/listN = data[] y size de la lektor del jugador (para el ángulo "char equivocado")
//   ccFn/appearance= createComponents + appearance (SOLO se usan si kFixHostAiNull==true)
// EN: Reads the host AI state and (without enabling anything while kFixHostAiNull=false) looks for an
//     alternative char with a valid AI in the player list. READ-ONLY unless the flag is enabled.
//       chr = host char (data[0]); aiVtblAbs = modBase + kAIVtableRVA (known AI vtable to validate);
//       listData/listN = data[] and size of the player lektor; ccFn/appearance = createComponents +
//       appearance (ONLY used if kFixHostAiNull==true).
static void SEH_DiagHostAiNull(uintptr_t chr, uintptr_t aiVtblAbs,
                               uintptr_t listData, int listN,
                               CreateComponentsFn ccFn, void* appearance,
                               AiNullFixSnapshot* out) {
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        return true;
    };
    __try {
        if (!isHeap(chr)) return;
        out->hostChar = chr;
        uintptr_t vtbl = 0;

        // Estado base del host: movement (debe existir), body, y el AI (síntoma).
        // EN: Base host state: movement (must exist), body, and the AI (the symptom).
        uintptr_t mv = 0, bd = 0, ai = 0;
        Memory::Read(chr + 0x640, mv); out->movement = mv;
        Memory::Read(chr + 0x648, bd); out->body     = bd;
        Memory::Read(chr + 0x650, ai); out->aiPre    = ai;
        if (isHeap(ai) && Memory::Read(ai, vtbl) && vtbl == aiVtblAbs) {
            out->aiPreVtblOk = 1;
            out->alreadyHad  = 1;       // el host YA tiene AI válido → no es la causa (idempotencia).
            out->aiPost      = ai;
            out->aiPostVtblOk = 1;
        }

        // ── Ángulo "char equivocado": ¿hay OTRO char en la lista del jugador con AI válido? ──
        // Recorre data[1..N-1] (data[0] es el host) y reporta el primero con vtable de AI correcta.
        // EN: ── "Wrong char" angle: is there ANOTHER char in the player list with a valid AI? ──
        //     Walks data[0..N-1] skipping the host and reports the first with the correct AI vtable.
        out->listSize = listN;
        if (isHeap(listData)) {
            int cap = listN; if (cap > 32) cap = 32; // techo de seguridad
            for (int i = 0; i < cap; i++) {
                uintptr_t ci = 0;
                if (!Memory::Read(listData + (uintptr_t)i * 8, ci) || !isHeap(ci)) continue;
                if (ci == chr) continue;
                uintptr_t aii = 0, cvt = 0;
                if (!Memory::Read(ci + 0x650, aii) || !isHeap(aii)) continue;
                if (Memory::Read(aii, cvt) && cvt == aiVtblAbs) {
                    out->altIndex = i; out->altChar = ci; out->altAi = aii;
                    break;
                }
            }
        }

        // ── Vía de creación de AI (DESACTIVADA mientras kFixHostAiNull==false) ──
        // Solo se ejecutaría si: flag activo + host SIN AI + appearance válido. createComponents
        // RE-ALOCA body/movement (no idempotente) → reservado, no se usa sin confirmar firma segura.
        // EN: ── AI creation path (DISABLED while kFixHostAiNull==false) ── It would only run if: flag on +
        //     host WITHOUT AI + valid appearance. createComponents RE-ALLOCATES body/movement (not idempotent).
        if (kFixHostAiNull && ccFn && appearance && out->aiPre == 0 && isHeap((uintptr_t)appearance)) {
            ccFn(reinterpret_cast<void*>(chr), appearance);
            out->called = 1;
            uintptr_t post = 0;
            if (Memory::Read(chr + 0x650, post)) {
                out->aiPost = post;
                if (isHeap(post) && Memory::Read(post, vtbl))
                    out->aiPostVtblOk = (vtbl == aiVtblAbs) ? 1 : 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->called = -1;
    }
}

// Orquesta el FIX-AI-NULL. Mismo patrón que FixHostCombatClassTick: throttle 250ms, one-shot
// re-armable, SOLO host, en el HILO DE LÓGICA. Con kFixHostAiNull=false NO escribe memoria del
// juego: solo confirma el síntoma y reporta si hay un char alternativo con AI válido (ángulo
// "char equivocado"). Corre ANTES del FIX-COMBATCLASS/FIX-PLATOON (que ASUMEN AI!=NULL).
// EN: Drives FIX-AI-NULL. Same pattern as FixHostCombatClassTick: 250 ms throttle, re-armable
//     one-shot, host ONLY, on the LOGIC THREAD. With kFixHostAiNull=false it does NOT write game
//     memory: it only confirms the symptom and reports an alternative char with a valid AI. Runs
//     BEFORE FIX-COMBATCLASS/FIX-PLATOON (which ASSUME AI!=NULL).
// ES: AVISO: FixHostAiNullTick y ResetHostAiNullFix no se llaman desde ningún sitio del proyecto
//     (código muerto / solo diagnóstico preparado); el FIX-AI-NULL no llega a ejecutarse.
// EN: WARNING: FixHostAiNullTick and ResetHostAiNullFix are not called from anywhere in the project
//     (dead code / prepared diagnostic only); FIX-AI-NULL never actually runs.
static void FixHostAiNullTick(Core& core) {
    if (s_aiNullFixDone.load()) return;                  // ya resuelto / confirmado
    if (s_aiNullAttempts >= AINULL_MAX_ATTEMPTS) return;

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastAiNullTry < std::chrono::milliseconds(250)) return;
    s_lastAiNullTry = now;
    s_aiNullAttempts++;

    uintptr_t modBase = Memory::GetModuleBase();
    if (modBase == 0) return;

    uintptr_t hostChar = game::GetPlayerPrimaryCharacterDirect();
    if (hostChar <= 0x10000) return;                     // char aún no resoluble → reintentar

    const uintptr_t aiVtblAbs = modBase + 0x16FA3E8;     // vtable AI (char+0x650), CombatStructSnapshot::kAIVtableRVA
    // ES: RVA 0x16FA3E8 = vtable de la clase AI dentro de kenshi_x64.exe (sirve para validar que el
    //     puntero de char+0x650 es de verdad un objeto AI).
    // EN: RVA 0x16FA3E8 = vtable of the AI class inside kenshi_x64.exe (used to check that the pointer at
    //     char+0x650 really is an AI object).

    // Resolver data[] y size de la lista del jugador para el ángulo "char equivocado".
    // Mismo camino ROBUSTO que GetPlayerPrimaryCharacterDirect / DetectHostSquadCloneTick:
    //   GameWorld → +player (PI). El lektor playerCharacters está EMBEBIDO en PI con offsets
    //   ABSOLUTOS (verificado por RTTI + lecturas en vivo 2026-07-14):
    //     size = *(uint32*)(PI + 0x2B8)   ·   data = *(Character**)(PI + 0x2C0)
    // PI+0x2B0 es un puntero opaco (vtable/alloc interno), NO la base de una sub-estructura con
    // size/data relativos: por eso NO se dereferencia ni se le suman +0x08/+0x10.
    //
    // BUG que se corrige aquí (idéntico al ya arreglado en DetectHostSquadCloneTick): el guard de
    // puntero anterior ("> 0x10000") NO excluía el rango del módulo. Con GameWorld EMBEBIDO,
    // *(gwAddr) es el vtable in-module del objeto; el guard débil lo aceptaba como 'gw', corrompía
    // la cadena gw→pi y dejaba listData=0 en bucle. El guard correcto (heap-ptr REAL: alineado y
    // FUERA del rango del módulo) es el que usan todos los resolvers que funcionan.
    // EN: Resolve data[] and size of the player list for the "wrong char" angle. Same ROBUST path as
    //     GetPlayerPrimaryCharacterDirect / DetectHostSquadCloneTick: GameWorld → +player (PI). The
    //     playerCharacters lektor is EMBEDDED in PI with ABSOLUTE offsets (verified by RTTI + live reads
    //     2026-07-14): size = *(uint32*)(PI + 0x2B8), data = *(Character**)(PI + 0x2C0). PI+0x2B0 is an
    //     opaque pointer (vtable/internal alloc), NOT the base of a sub-structure: it is not dereferenced.
    //     BUG fixed here (same as in DetectHostSquadCloneTick): the old pointer guard ("> 0x10000") did
    //     not exclude the module range; with an EMBEDDED GameWorld, *(gwAddr) is the in-module vtable, the
    //     weak guard accepted it as 'gw', broke the gw→pi chain and left listData=0 forever. The correct
    //     guard (REAL heap pointer: aligned and OUTSIDE the module) is the one all working resolvers use.
    uintptr_t listData = 0; int listN = 0;
    {
        uintptr_t gwAddr = game::GetResolvedGameWorld();
        if (gwAddr != 0) {
            // Rango del módulo para distinguir un heap-ptr del juego de un puntero in-module (vtable).
            // (modBase ya validado != 0 arriba.) Tamaño real vía cabecera PE, con fallback de 64MB.
            // EN: Module range to tell a game heap pointer from an in-module pointer (vtable). Real size from the
            //     PE header, 64 MB fallback.
            size_t modSize = 0x4000000; // 64MB fallback
            {
                auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
                if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
                    if (nt->Signature == IMAGE_NT_SIGNATURE)
                        modSize = nt->OptionalHeader.SizeOfImage;
                }
            }
            // heap-ptr del juego: por encima de 64KB, alineado a 8 y FUERA del rango del módulo.
            // EN: Game heap pointer: above 64 KB, 8-byte aligned and OUTSIDE the module range.
            auto isHeap = [modBase, modSize](uintptr_t v) -> bool {
                if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
                if ((v & 0x7) != 0) return false;
                if (v >= modBase && v < modBase + modSize) return false; // vtable/objeto in-module
                return true;
            };

            const auto& off = game::GetOffsets();
            // GameWorld: caso (a) puntero a GameWorld (deref una vez) vs (b) objeto embebido.
            // EN: GameWorld: case (a) pointer to GameWorld (deref once) vs (b) embedded object.
            uintptr_t gw = 0;
            if (!(Memory::Read(gwAddr, gw) && isHeap(gw))) gw = gwAddr;

            uintptr_t pi = 0;
            if (Memory::Read(gw + off.world.player, pi) && isHeap(pi)) {
                // Lektor embebido: size@PI+0x2B8 (uint32) · data@PI+0x2C0 (Character**). ABSOLUTO.
                // EN: Embedded lektor: size@PI+0x2B8 (uint32) · data@PI+0x2C0 (Character**). ABSOLUTE.
                uint32_t sz = 0; uintptr_t dp = 0;
                if (Memory::Read(pi + 0x2B8, sz) && Memory::Read(pi + 0x2C0, dp)) {
                    if (sz > 0 && sz <= 100000 && isHeap(dp)) { listData = dp; listN = (int)sz; }
                }
            }
        }
    }

    // appearance NO se resuelve (la vía de creación está tras el flag, que está en false).
    // EN: appearance is NOT resolved (the creation path is behind the flag, which is false).
    AiNullFixSnapshot fx{};
    SEH_DiagHostAiNull(hostChar, aiVtblAbs, listData, listN,
                       /*ccFn=*/nullptr, /*appearance=*/nullptr, &fx);

    if (fx.alreadyHad) {
        // El host YA tiene AI válido → NO es la causa (o ya se resolvió). Cerrar one-shot.
        // EN: The host ALREADY has a valid AI → not the cause (or already solved). Close the one-shot.
        s_aiNullFixDone.store(true);
        spdlog::info("[FIX-AI-NULL] host char=0x{:X} ya tiene AI VÁLIDO (char+0x650=0x{:X}, vtblOk=1) "
                     "→ no aplica (idempotente). El combate no se bloquea por AI nulo en este char.",
                     fx.hostChar, fx.aiPre);
        return;
    }

    if (fx.called == 1) {
        // Solo posible con kFixHostAiNull==true (hoy inalcanzable). Registrado por completitud.
        // EN: Only possible with kFixHostAiNull==true (unreachable today). Logged for completeness.
        if (fx.aiPostVtblOk) {
            s_aiNullFixDone.store(true);
            spdlog::info("[FIX-AI-NULL] APLICADO ✓ char=0x{:X} | char+0x650: 0x{:X} -> 0x{:X} (vtblOk=1) "
                         "vía createComponents 0x62AF50 (intento {}).",
                         fx.hostChar, fx.aiPre, fx.aiPost, s_aiNullAttempts);
        } else {
            spdlog::warn("[FIX-AI-NULL] createComponents NO dejó un AI válido (post=0x{:X}) — revisar "
                         "appearance/firma. char=0x{:X}.", fx.aiPost, fx.hostChar);
        }
        return;
    }
    if (fx.called == -1) {
        if (s_aiNullAttempts <= 5 || s_aiNullAttempts % 60 == 0)
            spdlog::warn("[FIX-AI-NULL] excepción durante el diagnóstico (char=0x{:X}) — reintento {}.",
                         fx.hostChar, s_aiNullAttempts);
        return;
    }

    // Flag en false → SOLO diagnóstico. Loguear el síntoma + el ángulo "char equivocado".
    // ANTES (char+0x650) y, conceptualmente, DESPUÉS (igual: no se tocó memoria).
    // EN: Flag false → diagnosis ONLY. Log the symptom + the "wrong char" angle, BEFORE (char+0x650)
    //     and, conceptually, AFTER (the same: memory was not touched).
    if (s_aiNullAttempts <= 5 || s_aiNullAttempts % 120 == 0) {
        const char* verdict;
        if (fx.aiPre != 0 && !fx.aiPreVtblOk)
            verdict = "char+0x650 != NULL pero vtable de AI desconocida → objeto inesperado (re-examinar)";
        else if (fx.altIndex >= 0)
            verdict = "*** AI del host NULL, PERO data[altIndex] SÍ tiene AI válido → el mod reclama el "
                      "CHAR EQUIVOCADO. FIX RECOMENDADO (seguro): reclamar data[altIndex] en vez de data[0] "
                      "en GetPlayerPrimaryCharacterDirect/ClaimHostPrimaryCharacter (NO crear AI a mano) ***";
        else
            verdict = "AI del host NULL y NINGÚN char de la lista tiene AI válido → la creación de AI no es "
                      "segura sin appearance/firma confirmada. Mantener kFixHostAiNull=false (no crashear).";

        spdlog::info("[FIX-AI-NULL] DIAG host char=0x{:X} | char+0x650(AI) ANTES=0x{:X} vtblOk={} | "
                     "DESPUÉS=0x{:X} (sin tocar: flag off) | move(+0x640)=0x{:X} body(+0x648)=0x{:X} | "
                     "listSize={} altIndex={} altChar=0x{:X} altAi=0x{:X} (intento {})",
                     fx.hostChar, fx.aiPre, fx.aiPreVtblOk, fx.aiPost, fx.movement, fx.body,
                     fx.listSize, fx.altIndex, fx.altChar, fx.altAi, s_aiNullAttempts);
        spdlog::info("[FIX-AI-NULL] ==> {}", verdict);
    }
    if (s_aiNullAttempts == AINULL_MAX_ATTEMPTS) {
        spdlog::warn("[FIX-AI-NULL] diagnóstico agotado tras {} intentos (kFixHostAiNull=false: NO se "
                     "creó AI por seguridad). Si altIndex>=0 en los logs → reclamar ese char; si no → "
                     "resolver el appearance (GameDataCopyStandalone*) antes de activar el flag.",
                     s_aiNullAttempts);
    }
}

// ══════════════════════════════════════════════════════════════════════════════════
// [DIAG-CLONESQUAD] — CAMA (GET_OUT_OF_BED): el host tiene un CharacterHuman DUPLICADO
//                     dentro de su propio squad (causa raíz sección 21 de la wiki, 2026-07-13).
// ══════════════════════════════════════════════════════════════════════════════════
// CAUSA (RE en vivo, bug reproducido con el host durmiendo, PID 552): el ActivePlatoon del
// host contiene DOS instancias "Player 1" derivadas del MISMO GameData template:
//   - la REAL, controlada por el jugador  (char+0x0C == 0),
//   - un CLON no controlado                (char+0x0C == 1) — mismo char+0x40 (template),
//     misma facción, mismo char+0x658 (ActivePlatoon). Su AI sí recibe tick (pertenece al
//     Platoon del host), y cuando "duerme" en una cama su Task GET_OUT_OF_BED (0x74) se
//     reencola sin fin (~86.910 veces) porque nadie ejecuta la acción de levantarse sobre él.
//
// ESTA PASADA ES SOLO DIAGNÓSTICO (lectura + log), NO ejecuta despawn. Motivo (decisión de
// seguridad, ver resumen del executor 2026-07-13): NO existe en el proyecto ninguna función
// NATIVA de despawn/eliminación de personaje reutilizable. El único "despawn" del proyecto
// (packet_handler.cpp SEH_DespawnCleanup) solo teletransporta bajo tierra a chars REMOTOS
// para ocultarlos + limpia el registro del mod; NO retira el char del mundo/squad ni cancela
// su Task, así que aplicarlo al clon del host (que comparte template con el char real) sería
// arriesgado y NO resolvería el bucle. Como esto es PERSISTENCIA de personajes (no un flag),
// se implementa solo la detección; la acción de despawn queda pendiente de una sesión de RE
// que localice la función nativa segura de retirada de miembro (candidatas a investigar:
// SquadAddMember 0x928423 / la ruta de reclamo del host que crea el clon — evitar la
// duplicación de origen, opción preferente 21.6.1). NUNCA escribir char+0x5BC (isDead) a mano.
//
// SEGURIDAD/THREAD: idéntico patrón que FixHostAiNullTick — SOLO host, HILO DE LÓGICA
// (OnGameTick), bajo SEH en helper POD (sin objetos C++ en el __try → evita C2712). One-shot
// re-armable (se re-arma en recarga de save / nueva conexión), throttle ~2s.
// EN: [DIAG-CLONESQUAD] — BED bug (GET_OUT_OF_BED): the host has a DUPLICATED CharacterHuman inside
//     its own squad (root cause, wiki section 21, 2026-07-13).
//     CAUSE (live RE, bug reproduced with the host sleeping): the host ActivePlatoon holds TWO
//       "Player 1" instances derived from the SAME GameData template: the REAL one, player controlled,
//       and an uncontrolled CLONE. The clone AI does get ticked, and when it "sleeps" in a bed its
//       GET_OUT_OF_BED Task (0x74) is re-queued forever (~86,910 times) because nobody runs the get-up
//       action on it.
//     THIS PASS IS DIAGNOSTIC ONLY (read + log), no despawn: there is no reusable NATIVE despawn
//       function in the project (packet_handler SEH_DespawnCleanup only teleports REMOTE chars
//       underground and clears the mod registry). The despawn action was left for an RE session that
//       finds a safe native member-removal function (see [FIX-CLONESQUAD-DESPAWN] below). NEVER write
//       char+0x5BC (isDead) by hand.
//     SAFETY/THREAD: host ONLY, LOGIC THREAD, under SEH in a POD helper. Re-armable one-shot, ~2 s
//       throttle.
//     NOTE: the criterion described above (char+0x0C == 1 and same ActivePlatoon) is the OLD one; the
//       code below now uses "same GameData template as data[0]" (see SEH_DiagHostSquadClone).
// ES: NOTA: el criterio descrito arriba (char+0x0C == 1 y mismo ActivePlatoon) es el ANTIGUO; el código
//     de abajo usa ya "mismo GameData template que data[0]" (ver SEH_DiagHostSquadClone). El
//     comentario de ResetHostSquadCloneDetect en core.h describe también el criterio antiguo.

// Toggle: detección pura (solo lectura + log). Default true — jamás escribe memoria del juego.
// EN: Toggle: pure detection (read + log only). Default true — never writes game memory.
static constexpr bool kDetectHostSquadClone = true;

// Estado one-shot re-armable del [DIAG-CLONESQUAD].
// EN: Re-armable one-shot state of [DIAG-CLONESQUAD] (~3 min at a 2 s throttle).
static std::atomic<bool> s_squadCloneDone{false};
static int  s_squadCloneAttempts = 0;
static auto s_lastSquadCloneTry  = std::chrono::steady_clock::now();
static constexpr int SQUADCLONE_MAX_ATTEMPTS = 90; // ~3 min a throttle 2s (roster ya poblado mucho antes)

// Re-arma el [DIAG-CLONESQUAD] para una partida/conexión nueva (mismo patrón que el resto).
// EN: Re-arms [DIAG-CLONESQUAD] for a new game/connection (same pattern as the rest).
void ResetHostSquadCloneDetect() {
    s_squadCloneDone.store(false);
    s_squadCloneAttempts = 0;
    s_lastSquadCloneTry = std::chrono::steady_clock::now();
}

// Snapshot POD del escaneo (vive dentro del __try; sin objetos C++ → evita C2712).
// EN: POD snapshot of the scan (lives inside __try; no C++ objects → avoids C2712). Holds the real host
//     (data[0]) with its mark (char+0x0C), GameData template (char+0x40) and ActivePlatoon (char+0x658),
//     the native list (data PI+0x2C0 / size PI+0x2B8), and the detected clone: pointer, mark,
//     GameData, AI (char+0x650), ActivePlatoon, list slot and cached position (char+0x48).
struct SquadCloneSnapshot {
    uintptr_t hostChar          = 0;  // char controlado real (data[0] de la lista PI+0x2C0)
    int       hostMark          = -1; // char+0x0C del host controlado (esperado 0)
    uintptr_t hostGameData      = 0;  // char+0x40 (GameData template) del host controlado
    uintptr_t hostActivePlatoon = 0;  // char+0x658 (ActivePlatoon) del host controlado
    uintptr_t listData          = 0;  // PI+0x2C0 — buffer nativo Character** (data[i]=*(listData+i*8))
    int       memberCount       = 0;  // PI+0x2B8 — size de la lista playerCharacters (nº de data[i])
    int       scanned           = 0;  // miembros efectivamente recorridos
    // Duplicado detectado (si lo hay) — NUEVO criterio: data[i] (i>0) con GameData IDÉNTICO a data[0].
    uintptr_t cloneChar          = 0; // data[i] (i>0) con char+0x40 == GameData del host
    int       cloneMark          = -1; // char+0x0C del clon (el real tenía 0, NO 1 → ya no se exige)
    uintptr_t cloneGameData      = 0;
    uintptr_t cloneAi            = 0; // clon+0x650 (la AI del bucle GET_OUT_OF_BED)
    uintptr_t cloneActivePlatoon = 0; // clon+0x658 (DISTINTO al del host → ya no se exige que coincida)
    int       cloneSlot          = -1; // índice i dentro de la lista nativa del jugador
    float     clonePosX          = 0.0f; // clon+0x48 (Vec3 cacheado read-only) — solo para el log
    float     clonePosY          = 0.0f;
    float     clonePosZ          = 0.0f;
    int       failReason         = 0;  // 0 ok · -1 excepción · -2 roster no resoluble
};

// Escanea la lista NATIVA de personajes del jugador buscando el clon.
// SOLO LECTURA. Fuente (confirmada por lectura en vivo 2026-07-14, offsets de PlayerInterface):
//   PI+0x2B8 = size (uint32) · PI+0x2C0 = data (Character**) · data[i] = *(listData + i*8).
// El caller resuelve listData/listN (GameWorld+0x580 PI → size@PI+0x2B8, data@PI+0x2C0 ABSOLUTOS;
// PI+0x2B0 NO se dereferencia)
// FUERA del __try. data[0] es el host controlado real (== hostChar). Se recorre TODA la lista, no
// solo el ActivePlatoon del host: el clon real vive en OTRO ActivePlatoon distinto, por eso el
// detector antiguo (solo ActivePlatoon del host, mark==1, mismo platoon) nunca lo encontraba.
// EN: Scans the player NATIVE character list looking for the clone. READ ONLY.
//     Source (confirmed by live read 2026-07-14, PlayerInterface offsets): PI+0x2B8 = size (uint32),
//     PI+0x2C0 = data (Character**), data[i] = *(listData + i*8). The caller resolves listData/listN
//     OUTSIDE the __try (PI+0x2B0 is NOT dereferenced). data[0] is the real controlled host. The WHOLE
//     list is walked, not only the host ActivePlatoon: the real clone lives in ANOTHER ActivePlatoon,
//     which is why the old detector (host ActivePlatoon only, mark==1, same platoon) never found it.
static void SEH_DiagHostSquadClone(uintptr_t hostChar, int gameDataOff,
                                   uintptr_t listData, int listN, SquadCloneSnapshot* out) {
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        return true;
    };
    __try {
        if (!isHeap(hostChar)) { out->failReason = -2; return; }
        out->hostChar = hostChar;

        // Marcador controlado/clon del host real (char+0x0C, low byte; esperado 0). Solo informativo.
        // EN: Controlled/clone marker of the real host (char+0x0C, low byte; expected 0). Informative only.
        uint32_t hmarkRaw = 0xFFFFFFFF;
        Memory::Read(hostChar + 0x0C, hmarkRaw);
        out->hostMark = (int)(hmarkRaw & 0xFF);

        // Template (GameData*, char+0x40) y ActivePlatoon (char+0x658) del host real.
        // EN: Template (GameData*, char+0x40) and ActivePlatoon (char+0x658) of the real host.
        uintptr_t hgd = 0, hap = 0;
        Memory::Read(hostChar + gameDataOff, hgd); out->hostGameData      = hgd; // +0x40
        Memory::Read(hostChar + 0x658, hap);       out->hostActivePlatoon = hap; // char+0x658
        // Sin GameData del host no hay comparación posible (el ActivePlatoon ya NO es requisito).
        // EN: Without the host GameData no comparison is possible (ActivePlatoon is no longer required).
        if (!isHeap(hgd)) { out->failReason = -2; return; }

        // Lista nativa del jugador (PI+0x2C0 data / PI+0x2B8 size), ya resuelta por el caller.
        // EN: Player native list (PI+0x2C0 data / PI+0x2B8 size), already resolved by the caller.
        out->listData    = listData;
        out->memberCount = listN;
        if (!isHeap(listData) || listN <= 0 || listN > 4096) { out->failReason = -2; return; }

        int cap = listN; if (cap > 256) cap = 256; // techo de seguridad
        for (int i = 1; i < cap; i++) {            // i>0: data[0] es el host, nunca "el clon"
            uintptr_t m = 0;
            if (!Memory::Read(listData + (uintptr_t)i * 8, m) || !isHeap(m)) continue;
            out->scanned++;
            if (m == hostChar) continue; // el propio host real (por si apareciera repetido)

            uintptr_t mgd = 0;
            Memory::Read(m + gameDataOff, mgd); // GameData del candidato (char+0x40)

            // NUEVO CRITERIO (más robusto que el antiguo): mismo GameData template que data[0] (host).
            //   Se ELIMINAN las exigencias de mark(+0x0C)==1 y de "mismo ActivePlatoon": ambas se
            //   demostraron NO fiables esta noche (el clon real tenía mark==0 y otro ActivePlatoon).
            // EN: NEW CRITERION (more robust than the old one): same GameData template as data[0] (host).
            //     The requirements mark(+0x0C)==1 and "same ActivePlatoon" are DROPPED: both proved unreliable
            //     (the real clone had mark==0 and a different ActivePlatoon).
            if (isHeap(mgd) && mgd == hgd) {
                uint32_t mmarkRaw = 0xFFFFFFFF; uintptr_t mai = 0, map = 0;
                Memory::Read(m + 0x0C, mmarkRaw);
                Memory::Read(m + 0x650, mai); // AI del clon (la del bucle GET_OUT_OF_BED)
                Memory::Read(m + 0x658, map); // ActivePlatoon del clon (distinto al del host)
                // Posición cacheada read-only del clon (char+0x48 = Vec3 x,y,z) — barata, solo log.
                // EN: Clone cached read-only position (char+0x48 = Vec3 x,y,z) — cheap, log only.
                Memory::Read(m + 0x48 + 0x0, out->clonePosX);
                Memory::Read(m + 0x48 + 0x4, out->clonePosY);
                Memory::Read(m + 0x48 + 0x8, out->clonePosZ);
                out->cloneChar          = m;
                out->cloneMark          = (int)(mmarkRaw & 0xFF);
                out->cloneGameData      = mgd;
                out->cloneAi            = mai;
                out->cloneActivePlatoon = map;
                out->cloneSlot          = i;
                break; // uno basta
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->failReason = -1;
    }
}

// Orquesta el [DIAG-CLONESQUAD]. Mismo patrón que FixHostAiNullTick: throttle ~2s, one-shot
// re-armable, SOLO host, en el HILO DE LÓGICA. NO escribe memoria del juego (detección pura).
// EN: Drives [DIAG-CLONESQUAD]. Same pattern as FixHostAiNullTick: ~2 s throttle, re-armable one-shot,
//     host ONLY, on the LOGIC THREAD. Does NOT write game memory (pure detection).
static void DetectHostSquadCloneTick(Core& core) {
    if (!kDetectHostSquadClone) return;
    if (s_squadCloneDone.load()) return;                     // ya detectado y logueado (estable)
    if (s_squadCloneAttempts >= SQUADCLONE_MAX_ATTEMPTS) return;

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastSquadCloneTry < std::chrono::milliseconds(2000)) return; // throttle ~2s
    s_lastSquadCloneTry = now;
    s_squadCloneAttempts++;

    uintptr_t hostChar = game::GetPlayerPrimaryCharacterDirect();
    if (hostChar <= 0x10000) return;                         // char aún no resoluble → reintentar

    const int gameDataOff = game::GetOffsets().character.gameDataPtr; // +0x40

    // Resolver la lista NATIVA del jugador FUERA del SEH, con el MISMO camino ROBUSTO que
    // GetPlayerPrimaryCharacterDirect / SEH_ReadPrimaryDiag ([DIAG-PRIMARY], que SÍ acierta):
    //   GameWorld → +0x580 (player) → PlayerInterface.
    // El lektor playerCharacters está EMBEBIDO en PlayerInterface: size y data son offsets
    // ABSOLUTOS desde PI (verificado por RTTI + lecturas directas en vivo 2026-07-14):
    //   size = *(uint32*)(PI + 0x2B8)      ·      data = *(Character**)(PI + 0x2C0)
    // PI+0x2B0 es un puntero opaco (vtable/alloc interno), NO la base de una sub-estructura con
    // size/data relativos: por eso NO se dereferencia ni se le suman +0x08/+0x10.
    //
    // BUG que se corrige aquí: el guard de puntero anterior ("> 0x10000") NO excluía el rango del
    // módulo. Con GameWorld EMBEBIDO (caso b), *(gwAddr) es el vtable in-module del objeto; el
    // guard débil lo aceptaba como 'gw', corrompía la cadena gw→pi y dejaba listData=0 en bucle
    // (60+ reintentos). El guard correcto (heap-ptr REAL: alineado y FUERA del módulo) es el que
    // usan todos los resolvers que funcionan. Si aún no resuelve, listData/listN=0 → el escaneo
    // marca failReason=-2 (roster no resoluble) y reintenta en el próximo throttle.
    // EN: Resolve the player NATIVE list OUTSIDE the SEH, with the SAME ROBUST path as
    //     GetPlayerPrimaryCharacterDirect / SEH_ReadPrimaryDiag ([DIAG-PRIMARY], which does work):
    //     GameWorld → +0x580 (player) → PlayerInterface. The playerCharacters lektor is EMBEDDED in
    //     PlayerInterface with ABSOLUTE offsets (verified by RTTI + live reads 2026-07-14):
    //     size = *(uint32*)(PI + 0x2B8), data = *(Character**)(PI + 0x2C0). PI+0x2B0 is an opaque pointer,
    //     NOT a sub-structure base: it is not dereferenced.
    //     BUG fixed here: the old pointer guard ("> 0x10000") did not exclude the module range. With an
    //     EMBEDDED GameWorld (case b), *(gwAddr) is the in-module vtable; the weak guard took it as 'gw',
    //     broke the gw→pi chain and left listData=0 in a loop (60+ retries). The correct guard (REAL heap
    //     pointer: aligned and OUTSIDE the module) is used by every working resolver. If it still does not
    //     resolve, listData/listN=0 → the scan reports failReason=-2 and retries on the next throttle.
    uintptr_t listData = 0; int listN = 0;
    {
        uintptr_t gwAddr = game::GetResolvedGameWorld();
        if (gwAddr != 0) {
            // Rango del módulo para distinguir un heap-ptr del juego de un puntero in-module (vtable).
            // EN: Module range to tell a game heap pointer from an in-module pointer (vtable).
            uintptr_t modBase = Memory::GetModuleBase();
            size_t    modSize = 0x4000000; // 64MB fallback
            {
                auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
                if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
                    if (nt->Signature == IMAGE_NT_SIGNATURE)
                        modSize = nt->OptionalHeader.SizeOfImage;
                }
            }
            // heap-ptr del juego: por encima de 64KB, alineado a 8 y FUERA del rango del módulo.
            // EN: Game heap pointer: above 64 KB, 8-byte aligned and OUTSIDE the module range.
            auto isHeap = [modBase, modSize](uintptr_t v) -> bool {
                if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
                if ((v & 0x7) != 0) return false;
                if (v >= modBase && v < modBase + modSize) return false; // vtable/objeto in-module
                return true;
            };

            const auto& off = game::GetOffsets();
            // GameWorld: caso (a) puntero a GameWorld (deref una vez) vs (b) objeto embebido.
            // EN: GameWorld: case (a) pointer to GameWorld (deref once) vs (b) embedded object.
            uintptr_t gw = 0;
            if (!(Memory::Read(gwAddr, gw) && isHeap(gw))) gw = gwAddr;

            uintptr_t pi = 0;
            if (Memory::Read(gw + off.world.player, pi) && isHeap(pi)) {
                // Lektor embebido: size@PI+0x2B8 (uint32) · data@PI+0x2C0 (Character**). ABSOLUTO.
                // EN: Embedded lektor: size@PI+0x2B8 (uint32) · data@PI+0x2C0 (Character**). ABSOLUTE.
                uint32_t sz = 0; uintptr_t dp = 0;
                if (Memory::Read(pi + 0x2B8, sz) && Memory::Read(pi + 0x2C0, dp)) {
                    if (sz > 0 && sz <= 100000 && isHeap(dp)) { listData = dp; listN = (int)sz; }
                }
            }
        }
    }

    SquadCloneSnapshot fx{};
    SEH_DiagHostSquadClone(hostChar, gameDataOff, listData, listN, &fx);

    if (fx.failReason == -1) {
        if (s_squadCloneAttempts <= 3 || s_squadCloneAttempts % 30 == 0)
            spdlog::warn("[DIAG-CLONESQUAD] excepción durante el escaneo (host=0x{:X}) — reintento {}.",
                         hostChar, s_squadCloneAttempts);
        return;
    }
    if (fx.failReason == -2) {
        // Roster aún no poblado (justo tras la carga) → reintentar en el próximo throttle.
        // EN: Roster not populated yet (right after loading) → retry on the next throttle.
        if (s_squadCloneAttempts <= 3 || s_squadCloneAttempts % 30 == 0)
            spdlog::info("[DIAG-CLONESQUAD] lista del jugador aún no resoluble "
                         "(hostChar=0x{:X} hostAP=0x{:X} listData=0x{:X} listN={}) — reintento {}.",
                         fx.hostChar, fx.hostActivePlatoon, fx.listData, fx.memberCount,
                         s_squadCloneAttempts);
        return;
    }

    if (fx.cloneChar != 0) {
        // DUPLICADO CONFIRMADO (nuevo criterio: mismo GameData que data[0]). Log UNA vez → one-shot.
        // EN: DUPLICATE CONFIRMED (new criterion: same GameData as data[0]). Log ONCE → one-shot.
        s_squadCloneDone.store(true);
        spdlog::warn("[DIAG-CLONESQUAD] *** DUPLICADO DETECTADO en la lista del jugador *** "
                     "hostReal=0x{:X}(mark={}) clon=0x{:X}(mark={}) slot={} | "
                     "GameData(+0x40) host=0x{:X} clon=0x{:X} (IDÉNTICO → criterio del fix) | "
                     "ActivePlatoon(+0x658) host=0x{:X} clon=0x{:X} (DISTINTO: el clon vive en otro "
                     "platoon, por eso el detector antiguo no lo veía) | clonAI(+0x650)=0x{:X} "
                     "(la del bucle GET_OUT_OF_BED) | clonPos=({:.1f},{:.1f},{:.1f}) | listN={}. "
                     "ACCIÓN DE DESPAWN NO EJECUTADA a propósito (solo diagnóstico; el despawn real "
                     "queda para otra fase). (intento {})",
                     fx.hostChar, fx.hostMark, fx.cloneChar, fx.cloneMark, fx.cloneSlot,
                     fx.hostGameData, fx.cloneGameData, fx.hostActivePlatoon, fx.cloneActivePlatoon,
                     fx.cloneAi, fx.clonePosX, fx.clonePosY, fx.clonePosZ, fx.memberCount,
                     s_squadCloneAttempts);
        return;
    }

    // No hay duplicado (todavía, o en absoluto). Log escaso mientras seguimos vigilando.
    // EN: No duplicate (yet, or at all). Sparse logging while we keep watching.
    if (s_squadCloneAttempts <= 3 || s_squadCloneAttempts % 30 == 0) {
        spdlog::info("[DIAG-CLONESQUAD] sin duplicado: host=0x{:X}(mark={}) GameData=0x{:X} "
                     "ActivePlatoon=0x{:X} listData=0x{:X} listN={} scanned={} (intento {}).",
                     fx.hostChar, fx.hostMark, fx.hostGameData, fx.hostActivePlatoon,
                     fx.listData, fx.memberCount, fx.scanned, s_squadCloneAttempts);
    }
    if (s_squadCloneAttempts == SQUADCLONE_MAX_ATTEMPTS) {
        spdlog::info("[DIAG-CLONESQUAD] vigilancia agotada tras {} intentos sin detectar duplicado "
                     "(squad del host con 1 solo 'Player 1', o roster no estabilizado). Se re-arma en "
                     "recarga de save / nueva conexión.", s_squadCloneAttempts);
    }
}

// ══════════════════════════════════════════════════════════════════════════════════
// [FIX-CLONESQUAD-DESPAWN] — DESPAWN REAL del CharacterHuman duplicado del squad del host.
// ══════════════════════════════════════════════════════════════════════════════════
// Complementa a [DIAG-CLONESQUAD] (que solo detecta+loguea): aquí SÍ se retira el clon del
// mundo. Diseño validado por RE exhaustiva (2026-07-14). Acción DESTRUCTIVA → protegida por:
//
//   1) GATING ANTI-FALSO-POSITIVO: NO se despawnea con una sola detección. Se exige estabilidad
//      ≥3 ticks CONSECUTIVOS con el MISMO puntero de clon y el MISMO GameData. Si el puntero
//      cambia o desaparece entre ticks (transitorio de carga/desconexión), el contador se
//      resetea a 0 y se espera de nuevo.
//
//   2) GUARDS DE SEGURIDAD (TODOS deben pasar, RE-CONFIRMADOS en vivo cada tick, sin cachear):
//        · clon != 0, heap-ptr válido y alineado.
//        · clon != host  y  clon != data[0]  (data[0] es SIEMPRE el host: jamás el slot 0).
//        · clon.GameData(+0x40) == host.GameData(+0x40)  (criterio del duplicado).
//        · clon.faction(+0x10) == host.faction(+0x10).
//        · clon.ActivePlatoon(+0x658) != host.ActivePlatoon(+0x658)  (el clon vive en OTRO
//          platoon; si coincidieran → aborta por sospecha).
//        · clon.isDead(+0x5BC) == 0  (si ya está muerto, no tocar).
//
//   3) ORDEN EXACTO DE LA ACCIÓN (crítico):
//        PRIMERO  erase del clon de la lektor PlayerInterface (data@PI+0x2C0 / size@PI+0x2B8):
//                 localizar el slot j con data[j]==cloneChar (comparación de PUNTERO, sin
//                 dereferenciar), compactar con memmove(data+j, data+j+1, (size-1-j)*8), poner a
//                 0 el último slot y escribir size-1 en PI+0x2B8. NO se toca capacidad, NO realloc,
//                 NO se libera memoria del array. Así ninguna lista sostiene el puntero cuando
//                 removeObject lo libere.
//        DESPUÉS  GameWorld::removeObject nativo (RVA 0x799AF0, AOB-verificada):
//                 void __fastcall(void* gw, void* obj, bool r8b, const char* reason).
//                 r8b=false CONFIRMADO (82% de 85 callers nativos; el análogo semántico
//                 "deleteDuplicatedBuildings" también usa false; con false el 'reason' se propaga
//                 al log). gw = GameWorld resuelto (embebido base+0x2134110, puntero directo).
//                 'reason' DEBE ser un literal static (vida útil persistente, no un buffer de stack).
//
// SEGURIDAD/THREAD: idéntico patrón que DetectHostSquadCloneTick/FixHostAiNullTick — SOLO host,
// HILO DE LÓGICA (OnGameTick), bajo SEH en helper POD (sin objetos C++ en el __try → evita
// C2712). One-shot re-armable (ResetHostSquadCloneDespawn en recarga de save / nueva conexión).
// EN: [FIX-CLONESQUAD-DESPAWN] — REAL DESPAWN of the duplicated CharacterHuman in the host squad.
//     Complements [DIAG-CLONESQUAD] (detect + log only): here the clone IS removed from the world.
//     Design validated by exhaustive RE (2026-07-14). DESTRUCTIVE action → protected by:
//       1) ANTI-FALSE-POSITIVE GATING: never despawn on a single detection. Requires stability of
//          ≥3 CONSECUTIVE ticks with the SAME clone pointer and the SAME GameData. If the pointer
//          changes or disappears between ticks (load/disconnect transient), the counter resets.
//       2) SAFETY GUARDS (ALL must pass, RE-CONFIRMED live every tick, never cached): clone != 0,
//          valid aligned heap pointer; clone != host and clone != data[0] (slot 0 is ALWAYS the host);
//          clone.GameData(+0x40) == host.GameData; clone.faction(+0x10) == host.faction;
//          clone.ActivePlatoon(+0x658) != host.ActivePlatoon (if equal → abort as suspicious);
//          clone.isDead(+0x5BC) == 0.
//       3) EXACT ACTION ORDER (critical): FIRST erase the clone from the PlayerInterface lektor
//          (data@PI+0x2C0 / size@PI+0x2B8): find slot j with data[j]==cloneChar (pointer compare,
//          no deref), compact with memmove, zero the last slot and write size-1 to PI+0x2B8 (no
//          capacity change, no realloc, no free) so no list holds the pointer when removeObject
//          frees it. THEN the native GameWorld::removeObject (RVA 0x799AF0, AOB-verified):
//          void __fastcall(void* gw, void* obj, bool r8b, const char* reason), r8b=false CONFIRMED
//          (82% of 85 native callers), gw = resolved GameWorld (embedded base+0x2134110). 'reason'
//          MUST be a static literal (persistent lifetime, not a stack buffer).
//     SAFETY/THREAD: host ONLY, LOGIC THREAD (OnGameTick), under SEH in a POD helper. Re-armable
//       one-shot (ResetHostSquadCloneDespawn on save reload / new connection).

// Toggle de seguridad. Acción DESTRUCTIVA (retira un char del mundo). Default true.
// EN: Safety toggle. DESTRUCTIVE action (removes a char from the world). Default true.
static constexpr bool kDespawnHostSquadClone = true;

// GameWorld::removeObject — RVA verificada por AOB (2026-07-14).
// EN: GameWorld::removeObject — RVA verified by AOB (2026-07-14). Native signature below.
static constexpr uintptr_t kRemoveObjectRVA = 0x799AF0;
using RemoveObjectFn = void(__fastcall*)(void* gw, void* obj, bool r8b, const char* reason);

// Estado one-shot re-armable + gating de estabilidad (≥3 ticks con el MISMO clon).
// EN: Re-armable one-shot state + stability gating (≥3 ticks with the SAME clone): consecutive stable
//     ticks, clone pointer and GameData seen on the previous tick, attempts and last try (~4 min at a
//     2 s throttle).
static std::atomic<bool> s_squadCloneDespawnDone{false};
static int       s_squadCloneDespawnStable   = 0;   // ticks consecutivos con el mismo clon+GameData
static uintptr_t s_squadCloneDespawnLastPtr  = 0;   // puntero del clon observado el tick anterior
static uintptr_t s_squadCloneDespawnLastGD   = 0;   // GameData(+0x40) del clon el tick anterior
static int       s_squadCloneDespawnAttempts = 0;
static auto      s_lastSquadCloneDespawnTry  = std::chrono::steady_clock::now();
static constexpr int SQUADCLONE_DESPAWN_STABLE_TICKS = 3;   // estabilidad mínima antes de actuar
static constexpr int SQUADCLONE_DESPAWN_MAX_ATTEMPTS = 120; // ~4 min a throttle 2s

// Re-arma el [FIX-CLONESQUAD-DESPAWN] para una partida/conexión nueva (mismo patrón que el resto).
// EN: Re-arms [FIX-CLONESQUAD-DESPAWN] for a new game/connection (same pattern as the rest).
void ResetHostSquadCloneDespawn() {
    s_squadCloneDespawnDone.store(false);
    s_squadCloneDespawnStable   = 0;
    s_squadCloneDespawnLastPtr  = 0;
    s_squadCloneDespawnLastGD   = 0;
    s_squadCloneDespawnAttempts = 0;
    s_lastSquadCloneDespawnTry  = std::chrono::steady_clock::now();
}

// Snapshot POD del despawn (vive dentro del __try; sin objetos C++ → evita C2712).
// EN: POD snapshot of the despawn (lives inside __try; no C++ objects → avoids C2712): host and clone
//     GameData (+0x40), faction (+0x10), ActivePlatoon (+0x658), clone isDead (+0x5BC), slot found,
//     live size before/after compacting, and flags guardsOk / erased / removed / failReason.
struct SquadCloneDespawnSnapshot {
    uintptr_t hostChar          = 0;
    uintptr_t hostGameData      = 0;  // +0x40
    uintptr_t hostFaction       = 0;  // +0x10
    uintptr_t hostActivePlatoon = 0;  // +0x658
    uintptr_t cloneChar         = 0;
    uintptr_t cloneGameData     = 0;  // +0x40 (== hostGameData si es el clon)
    uintptr_t cloneFaction      = 0;  // +0x10
    uintptr_t cloneActivePlatoon= 0;  // +0x658
    int       cloneIsDead       = -1; // +0x5BC (0=vivo, 1=muerto)
    int       cloneSlot         = -1; // índice hallado en el escaneo de re-localización
    int       curSize           = -1; // size re-leído en vivo (PI+0x2B8) al hacer commit
    int       newSize           = -1; // size tras compactar (curSize-1)
    int       guardsOk          = 0;  // 1 si TODOS los guards pasaron
    int       erased            = 0;  // 1 si se compactó la lista PlayerInterface
    int       removed           = 0;  // 1 si se llamó a removeObject
    int       failReason        = 0;  // 0 ok · -1 excepción · -2 no resoluble / abortado
};

// Re-resuelve el clon EN VIVO, valida TODOS los guards y (si commit && guards) ejecuta el
// despawn en el ORDEN EXACTO: erase de la lista PRIMERO, removeObject DESPUÉS. SOLO LECTURA si
// commit=false. Todo bajo SEH; POD, sin objetos C++ con destructor dentro del __try (evita C2712).
// El caller resuelve listData/listN/pi/gw FUERA del __try (mismo camino robusto que el detector).
// EN: Re-resolves the clone LIVE, validates ALL the guards and (if commit && guards) runs the despawn in
//     the EXACT ORDER: erase from the list FIRST, removeObject AFTER. READ ONLY if commit=false. All
//     under SEH; POD, no C++ objects with destructors inside __try (avoids C2712). The caller resolves
//     listData/listN/pi/gw OUTSIDE the __try (same robust path as the detector).
static void SEH_DespawnHostSquadClone(uintptr_t hostChar, int gameDataOff,
                                      uintptr_t listData, int listN, uintptr_t pi,
                                      uintptr_t gw, RemoveObjectFn removeFn,
                                      bool commit, SquadCloneDespawnSnapshot* out) {
    // heap-ptr del juego: por encima de 64KB y alineado a 8 (aquí NO se excluye el rango del
    // módulo a propósito: gw es el GameWorld EMBEBIDO base+0x2134110, in-module y válido).
    // EN: Game heap pointer: above 64 KB and 8-byte aligned (the module range is deliberately NOT excluded
    //     here: gw is the EMBEDDED GameWorld base+0x2134110, in-module and valid).
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        return true;
    };
    // Motivo del despawn: DEBE ser static (vida útil persistente). removeObject lo propaga al log;
    // un buffer de stack sería inválido tras retornar de esta función.
    // EN: Despawn reason: MUST be static (persistent lifetime). removeObject forwards it to the log; a
    //     stack buffer would be invalid after this function returns.
    static const char kReason[] = "kenshi-coop: despawn clon de squad del host";

    __try {
        if (!isHeap(hostChar)) { out->failReason = -2; return; }
        out->hostChar = hostChar;

        // Host: GameData(+0x40), faction(+0x10), ActivePlatoon(+0x658) — re-leídos en vivo ESTE tick.
        // EN: Host: GameData(+0x40), faction(+0x10), ActivePlatoon(+0x658) — re-read live THIS tick.
        uintptr_t hgd = 0, hfac = 0, hap = 0;
        Memory::Read(hostChar + gameDataOff, hgd);  out->hostGameData      = hgd;
        Memory::Read(hostChar + 0x10,        hfac); out->hostFaction       = hfac;
        Memory::Read(hostChar + 0x658,       hap);  out->hostActivePlatoon = hap;
        if (!isHeap(hgd)) { out->failReason = -2; return; } // sin GameData del host no hay comparación

        if (!isHeap(listData) || listN <= 0 || listN > 4096) { out->failReason = -2; return; }

        // data[0] es SIEMPRE el host: nunca candidato a despawn (guard duro más abajo).
        // EN: data[0] is ALWAYS the host: never a despawn candidate (hard guard below).
        uintptr_t data0 = 0;
        Memory::Read(listData, data0);

        // Re-localizar el clon por el criterio GameData-idéntico (i>0), igual que el detector.
        // EN: Re-locate the clone with the identical-GameData criterion (i>0), same as the detector.
        uintptr_t clone = 0, cgd = 0; int cslot = -1;
        int cap = listN; if (cap > 256) cap = 256; // techo de seguridad
        for (int i = 1; i < cap; i++) {            // i>0: data[0] es el host, nunca "el clon"
            uintptr_t m = 0;
            if (!Memory::Read(listData + (uintptr_t)i * 8, m) || !isHeap(m)) continue;
            if (m == hostChar) continue;           // el propio host (por si apareciera repetido)
            uintptr_t mgd = 0;
            Memory::Read(m + gameDataOff, mgd);    // GameData del candidato (char+0x40)
            if (isHeap(mgd) && mgd == hgd) { clone = m; cgd = mgd; cslot = i; break; }
        }
        if (clone == 0) { out->failReason = -2; return; } // clon no presente este tick → transitorio

        out->cloneChar     = clone;
        out->cloneGameData = cgd;
        out->cloneSlot     = cslot;

        // Guards del clon: faction(+0x10), ActivePlatoon(+0x658), isDead(+0x5BC) — en vivo.
        // EN: Clone guards: faction(+0x10), ActivePlatoon(+0x658), isDead(+0x5BC) — read live.
        uintptr_t cfac = 0, cap658 = 0; uint32_t deadRaw = 0xFFFFFFFF;
        Memory::Read(clone + 0x10,  cfac);    out->cloneFaction        = cfac;
        Memory::Read(clone + 0x658, cap658);  out->cloneActivePlatoon  = cap658;
        Memory::Read(clone + 0x5BC, deadRaw); out->cloneIsDead         = (int)(deadRaw & 0xFF);

        // ── GUARDS DE SEGURIDAD (TODOS deben pasar antes de actuar) ──
        // EN: ── SAFETY GUARDS (ALL must pass before acting) ── valid aligned heap pointer, not the host, not
        //     slot 0, identical GameData, same faction, DIFFERENT platoon (abort if equal), alive.
        bool ok = true;
        ok = ok && isHeap(clone);                       // heap-ptr válido y alineado
        ok = ok && (clone != hostChar);                 // no es el host
        ok = ok && (data0  != clone);                   // no es el slot 0 (host)
        ok = ok && (cgd == hgd);                        // GameData idéntico (re-confirmado este tick)
        ok = ok && isHeap(hfac) && (cfac == hfac);      // misma facción
        ok = ok && isHeap(hap) && isHeap(cap658) && (cap658 != hap); // platoon DISTINTO (si igual: abortar)
        ok = ok && (out->cloneIsDead == 0);             // vivo (si ya muerto, no tocar)
        out->guardsOk = ok ? 1 : 0;

        if (!commit || !ok) return; // solo lectura, o guards no superados → nada que mutar

        // Validar removeObject ANTES de tocar la lista: así erase y removeObject son atómicos
        // (ocurren ambos o ninguno). Evita dejar la lista compactada sin retirar el objeto.
        // EN: Validate removeObject BEFORE touching the list: erase and removeObject become atomic (both or
        //     none), so the list is never left compacted without removing the object.
        if (removeFn == nullptr || !isHeap(gw)) { out->failReason = -2; return; }

        // Re-leer el size AUTORITATIVO en vivo (PI+0x2B8) justo antes de compactar.
        // EN: Re-read the AUTHORITATIVE live size (PI+0x2B8) right before compacting.
        uint32_t curSz = 0;
        if (!Memory::Read(pi + 0x2B8, curSz) || curSz == 0 || curSz > 4096) {
            out->failReason = -2; return;
        }
        out->curSize = (int)curSz;

        // Buscar el slot j con data[j] == cloneChar (comparación de PUNTERO, sin dereferenciar).
        // EN: Find slot j with data[j] == cloneChar (POINTER comparison, no dereference).
        int j = -1;
        for (uint32_t k = 0; k < curSz; k++) {
            uintptr_t m = 0;
            if (Memory::Read(listData + (uintptr_t)k * 8, m) && m == clone) { j = (int)k; break; }
        }
        if (j <= 0) { out->failReason = -2; return; } // no hallado, o es el slot 0 (host) → abortar

        // ── PRIMERO: erase del clon de la lektor PlayerInterface (compactar sin realloc) ──
        // data[] es el buffer nativo Character** (in-process); la escritura va bajo este __try.
        // EN: ── FIRST: erase the clone from the PlayerInterface lektor (compact without realloc) ──
        //     data[] is the native Character** buffer (in-process); the write happens under this __try.
        auto* dataPtr = reinterpret_cast<uintptr_t*>(listData);
        size_t tail = (size_t)(curSz - 1 - (uint32_t)j);          // nº de slots a desplazar
        if (tail > 0) memmove(dataPtr + j, dataPtr + j + 1, tail * sizeof(uintptr_t));
        dataPtr[curSz - 1] = 0;                                    // limpiar el último slot
        Memory::Write(pi + 0x2B8, (uint32_t)(curSz - 1));         // nuevo size (NO se toca capacidad)
        out->newSize = (int)(curSz - 1);
        out->erased  = 1;

        // ── DESPUÉS: removeObject nativo (r8b=false CONFIRMADO). gw = GameWorld directo ──
        // EN: ── THEN: native removeObject (r8b=false CONFIRMED). gw = direct GameWorld ──
        removeFn(reinterpret_cast<void*>(gw), reinterpret_cast<void*>(clone),
                 /*r8b=*/false, kReason);
        out->removed = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->failReason = -1;
    }
}

// Orquesta el [FIX-CLONESQUAD-DESPAWN]. Mismo patrón que DetectHostSquadCloneTick: throttle ~2s,
// one-shot re-armable, SOLO host, HILO DE LÓGICA. Re-resuelve TODO en vivo cada tick (no cachea
// punteros entre ticks salvo el contador de estabilidad). Primera pasada solo-lectura para el
// gating; segunda pasada (commit) solo cuando la estabilidad llega a ≥3 ticks.
// EN: Drives [FIX-CLONESQUAD-DESPAWN]. Same pattern as DetectHostSquadCloneTick: ~2 s throttle,
//     re-armable one-shot, host ONLY, LOGIC THREAD. Re-resolves EVERYTHING live each tick (no pointers
//     cached between ticks except the stability counter). First pass read-only for the gating; second
//     pass (commit) only once stability reaches ≥3 ticks.
static void DespawnHostSquadCloneTick(Core& core) {
    if (!kDespawnHostSquadClone) return;
    if (s_squadCloneDespawnDone.load()) return;                   // ya despawneado (one-shot)
    if (s_squadCloneDespawnAttempts >= SQUADCLONE_DESPAWN_MAX_ATTEMPTS) return;

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastSquadCloneDespawnTry < std::chrono::milliseconds(2000)) return; // throttle ~2s
    s_lastSquadCloneDespawnTry = now;
    s_squadCloneDespawnAttempts++;

    uintptr_t hostChar = game::GetPlayerPrimaryCharacterDirect();
    if (hostChar <= 0x10000) {                                    // char aún no resoluble
        s_squadCloneDespawnStable = 0; s_squadCloneDespawnLastPtr = 0; s_squadCloneDespawnLastGD = 0;
        return;
    }

    const int gameDataOff = game::GetOffsets().character.gameDataPtr; // +0x40

    // Resolver PI, listData/listN y gw EN VIVO, con el MISMO camino ROBUSTO que
    // DetectHostSquadCloneTick (GameWorld → +player PI; size@PI+0x2B8, data@PI+0x2C0 ABSOLUTOS;
    // guard heap-ptr REAL que excluye el rango del módulo para no confundir un vtable in-module).
    // EN: Resolve PI, listData/listN and gw LIVE, with the SAME ROBUST path as DetectHostSquadCloneTick
    //     (GameWorld → +player PI; size@PI+0x2B8, data@PI+0x2C0 ABSOLUTE; REAL heap-pointer guard that
    //     excludes the module range so an in-module vtable is not mistaken).
    uintptr_t listData = 0; int listN = 0; uintptr_t pi = 0; uintptr_t gwResolved = 0;
    {
        uintptr_t gwAddr = game::GetResolvedGameWorld();
        if (gwAddr != 0) {
            uintptr_t modBase = Memory::GetModuleBase();
            size_t    modSize = 0x4000000; // 64MB fallback
            {
                auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
                if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(modBase + dos->e_lfanew);
                    if (nt->Signature == IMAGE_NT_SIGNATURE)
                        modSize = nt->OptionalHeader.SizeOfImage;
                }
            }
            auto isHeap = [modBase, modSize](uintptr_t v) -> bool {
                if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
                if ((v & 0x7) != 0) return false;
                if (v >= modBase && v < modBase + modSize) return false; // vtable/objeto in-module
                return true;
            };

            const auto& off = game::GetOffsets();
            // GameWorld: caso (a) puntero a GameWorld (deref una vez) vs (b) objeto embebido.
            uintptr_t gw = 0;
            if (!(Memory::Read(gwAddr, gw) && isHeap(gw))) gw = gwAddr;
            gwResolved = gw;

            uintptr_t piLocal = 0;
            if (Memory::Read(gw + off.world.player, piLocal) && isHeap(piLocal)) {
                pi = piLocal;
                uint32_t sz = 0; uintptr_t dp = 0;
                if (Memory::Read(pi + 0x2B8, sz) && Memory::Read(pi + 0x2C0, dp)) {
                    if (sz > 0 && sz <= 100000 && isHeap(dp)) { listData = dp; listN = (int)sz; }
                }
            }
        }
    }

    // Puntero a la función nativa removeObject (base + RVA, mismo estilo que el resto del mod).
    // EN: Pointer to the native removeObject function (base + RVA, same style as the rest of the mod).
    RemoveObjectFn removeFn = reinterpret_cast<RemoveObjectFn>(Memory::GetModuleBase() + kRemoveObjectRVA);

    // ── 1ª PASADA: SOLO LECTURA + guards (commit=false) → decide la estabilidad ──
    // EN: ── 1st PASS: READ ONLY + guards (commit=false) → decides stability ──
    SquadCloneDespawnSnapshot fx{};
    SEH_DespawnHostSquadClone(hostChar, gameDataOff, listData, listN, pi, gwResolved,
                              removeFn, /*commit=*/false, &fx);

    if (fx.failReason == -1) { // excepción → tratar como transitorio, resetear estabilidad
        s_squadCloneDespawnStable = 0; s_squadCloneDespawnLastPtr = 0; s_squadCloneDespawnLastGD = 0;
        if (s_squadCloneDespawnAttempts <= 3 || s_squadCloneDespawnAttempts % 30 == 0)
            spdlog::warn("[FIX-CLONESQUAD-DESPAWN] excepción durante la re-resolución (host=0x{:X}) — "
                         "reintento {}.", hostChar, s_squadCloneDespawnAttempts);
        return;
    }

    // Sin clon presente o guards no superados → transitorio: resetear el contador de estabilidad.
    // EN: No clone present or guards failed → transient: reset the stability counter.
    if (fx.cloneChar == 0 || fx.guardsOk != 1) {
        if (s_squadCloneDespawnStable != 0 &&
            (s_squadCloneDespawnAttempts <= 5 || s_squadCloneDespawnAttempts % 30 == 0))
            spdlog::info("[FIX-CLONESQUAD-DESPAWN] candidato inestable/ausente (clon=0x{:X} guardsOk={} "
                         "isDead={}) → estabilidad reseteada. (intento {})",
                         fx.cloneChar, fx.guardsOk, fx.cloneIsDead, s_squadCloneDespawnAttempts);
        s_squadCloneDespawnStable  = 0;
        s_squadCloneDespawnLastPtr = 0;
        s_squadCloneDespawnLastGD  = 0;
        return;
    }

    // Gating de estabilidad: MISMO puntero + MISMO GameData que el tick anterior → cuenta; si
    // cambió (candidato nuevo) → reinicia a 1 registrando el nuevo candidato.
    // EN: Stability gating: SAME pointer + SAME GameData as the previous tick → count up; if it changed
    //     (new candidate) → restart at 1 tracking the new candidate.
    if (fx.cloneChar == s_squadCloneDespawnLastPtr && fx.cloneGameData == s_squadCloneDespawnLastGD) {
        s_squadCloneDespawnStable++;
    } else {
        s_squadCloneDespawnLastPtr = fx.cloneChar;
        s_squadCloneDespawnLastGD  = fx.cloneGameData;
        s_squadCloneDespawnStable  = 1; // primera observación estable de este candidato
    }

    if (s_squadCloneDespawnStable < SQUADCLONE_DESPAWN_STABLE_TICKS) {
        spdlog::info("[FIX-CLONESQUAD-DESPAWN] clon 0x{:X} confirmado (slot={} faction=0x{:X} "
                     "platoon(clon)=0x{:X} platoon(host)=0x{:X}) estabilidad {}/{} — a la espera de "
                     "más ticks antes de despawnear. (intento {})",
                     fx.cloneChar, fx.cloneSlot, fx.cloneFaction, fx.cloneActivePlatoon,
                     fx.hostActivePlatoon, s_squadCloneDespawnStable, SQUADCLONE_DESPAWN_STABLE_TICKS,
                     s_squadCloneDespawnAttempts);
        return;
    }

    // ── ≥3 ticks estables → 2ª PASADA con COMMIT: re-confirma guards en vivo, ERASE luego removeObject ──
    // EN: ── ≥3 stable ticks → 2nd PASS with COMMIT: re-confirm guards live, ERASE then removeObject ──
    SquadCloneDespawnSnapshot cx{};
    SEH_DespawnHostSquadClone(hostChar, gameDataOff, listData, listN, pi, gwResolved,
                              removeFn, /*commit=*/true, &cx);

    if (cx.failReason == -1) { // excepción durante el commit → resetear y reintentar
        s_squadCloneDespawnStable = 0; s_squadCloneDespawnLastPtr = 0; s_squadCloneDespawnLastGD = 0;
        spdlog::warn("[FIX-CLONESQUAD-DESPAWN] excepción durante el COMMIT (host=0x{:X} clon=0x{:X}) — "
                     "estabilidad reseteada, reintento {}.", hostChar, fx.cloneChar,
                     s_squadCloneDespawnAttempts);
        return;
    }
    if (cx.guardsOk != 1 || cx.removed != 1) {
        // La re-confirmación en vivo (o la localización del slot) no fue segura entre pasadas →
        // abortar este tick, resetear la estabilidad y dejar que se re-verifique desde cero.
        // EN: The live re-confirmation (or the slot lookup) was not safe between passes → abort this tick,
        //     reset stability and let it re-verify from scratch.
        s_squadCloneDespawnStable = 0; s_squadCloneDespawnLastPtr = 0; s_squadCloneDespawnLastGD = 0;
        spdlog::warn("[FIX-CLONESQUAD-DESPAWN] COMMIT abortado (guardsOk={} erased={} removed={} "
                     "clon=0x{:X} failReason={}) — la re-confirmación en vivo no fue segura. Se "
                     "reintentará. (intento {})",
                     cx.guardsOk, cx.erased, cx.removed, cx.cloneChar, cx.failReason,
                     s_squadCloneDespawnAttempts);
        return;
    }

    // ── ÉXITO (one-shot). Erase de la lektor PRIMERO, removeObject DESPUÉS — confirmado ──
    // EN: ── SUCCESS (one-shot). Lektor erase FIRST, removeObject AFTER — confirmed ──
    s_squadCloneDespawnDone.store(true);
    spdlog::warn("[FIX-CLONESQUAD-DESPAWN] clon 0x{:X} despawneado (host 0x{:X} intacto) | slot={} "
                 "size {}→{} | faction=0x{:X} platoon(clon)=0x{:X} platoon(host)=0x{:X} | erase de "
                 "la lektor PlayerInterface PRIMERO, GameWorld::removeObject(0x{:X}) DESPUÉS "
                 "(r8b=false). (intento {})",
                 cx.cloneChar, cx.hostChar, cx.cloneSlot, cx.curSize, cx.newSize, cx.cloneFaction,
                 cx.cloneActivePlatoon, cx.hostActivePlatoon,
                 Memory::GetModuleBase() + kRemoveObjectRVA, s_squadCloneDespawnAttempts);
}

// ══════════════════════════════════════════════════════════════════════════════════
// [FIX-COMBATCLASS] — Causa raíz del combate: el char del host nace SIN CombatClass.
// ══════════════════════════════════════════════════════════════════════════════════
// CAUSA (RE de bytes, audit-12 + agente a0e98196): el spawn del mod entra por
// RootObjectFactory::create (0x583400) → process (0x581770). El gate de tipo en
// 0x582A5B (cmp [[desc+0x18]+0x50],1; jne 0x582B5E) SALTA giveBirth (0x62B210) si el
// descriptor NO es tipo==1. giveBirth→createComponents (0x62AF50)→CharBody::create
// (0x621460) es la ÚNICA cadena que escribe la CombatClass en CharBody+0x8 (en 0x6214EF).
// Al saltarse esa rama, el char tiene AI(+0x650)/CharBody(+0x648)/CharMovement(+0x640)/
// AnimationClass(+0x448) pero CombatClass = *(CharBody+0x8) = NULL → camina/mina/entrena
// (GOAP) pero NO ATACA (atacar usa la CombatClass: tick 0x5C67C0, attackTarget 0x5CB0A0,
// currentTarget@CombatClass+0x290 que escribe setAttackTarget 0x665580).
//
// FIX QUIRÚRGICO: el CharBody YA existe (createComponents lo crea; solo +0x8 es NULL),
// y los 5 args que CharBody::create necesita YA viven en el char (camina). Así que basta
// con llamar a CharBody::create(0x621460) sobre el char para que RELLENE la CombatClass.
// NO se rehace giveBirth (duplicaría/corrompería los subsistemas ya creados).
//
// Firma confirmada en bytes (game-reverse-engineer, Steam 1.0.68):
//   void CharBody::create(CharBody* this/*rcx=char+0x648*/, CharMovement* /*rdx=char+0x640*/,
//                         AI* /*r8=char+0x650*/, AnimationClass* /*r9=char+0x448*/,
//                         Character* /*[rsp+0x28]=char*/, CharStats* /*[rsp+0x30]=char+0x450*/);
//
// SEGURIDAD (verificado en bytes):
//   • NO idempotente: hace new(0x2F8) INCONDICIONAL y sobreescribe CharBody+0x8 a ciegas
//     (0x6214B0/0x6214EF, sin test previo). → GUARD OBLIGATORIO: solo llamar si +0x8==NULL,
//     o se filtra (leak) la CombatClass previa. Por eso comprobamos *(CharBody+0x8)==0 antes.
//   • No registra en GameWorld ni en listas globales; solo escribe dentro de CharBody
//     (+0x8/+0x10/+0x18/+0x20/+0x50/+0x58) y del objeto CombatClass nuevo. Sin estado
//     transitorio de spawn → seguro post-spawn.
//   • La CombatClass sale lista del create (no requiere activación posterior).
//   • Thread safety: se ejecuta en el HILO DE LÓGICA (OnGameTick), igual que FIX-CONTROL,
//     NO desde el callback de red (el asignador 'new' del juego no es thread-safe).
//
// Por prudencia (crear un subsistema sobre un char ya construido es delicado), va detrás de
// un TOGGLE por defecto OFF: primero se valida con el [DIAG-ATTACK] que la CombatClass es
// realmente NULL en el host y != NULL en los NPC; luego se activa kEnableCombatClassFix.
// EN: [FIX-COMBATCLASS] — Root cause of combat: the host char is born WITHOUT a CombatClass.
//     CAUSE (byte RE, audit-12): the mod spawn goes through RootObjectFactory::create (0x583400) →
//       process (0x581770). The type gate at 0x582A5B SKIPS giveBirth (0x62B210) if the descriptor is
//       NOT type==1. giveBirth→createComponents (0x62AF50)→CharBody::create (0x621460) is the ONLY
//       chain that writes the CombatClass into CharBody+0x8 (at 0x6214EF). With that branch skipped
//       the char has AI(+0x650)/CharBody(+0x648)/CharMovement(+0x640)/AnimationClass(+0x448) but
//       CombatClass = *(CharBody+0x8) = NULL → walks/mines/trains (GOAP) but does NOT ATTACK
//       (attacking uses the CombatClass: tick 0x5C67C0, attackTarget 0x5CB0A0,
//       currentTarget@CombatClass+0x290 written by setAttackTarget 0x665580).
//     SURGICAL FIX: the CharBody ALREADY exists (only +0x8 is NULL) and the 5 args CharBody::create
//       needs ALREADY live in the char. So calling CharBody::create(0x621460) on the char FILLS the
//       CombatClass. giveBirth is NOT redone (it would duplicate/corrupt existing subsystems).
//     Signature confirmed in bytes (Steam 1.0.68): see the prototype above (this=char+0x648,
//       CharMovement=char+0x640, AI=char+0x650, AnimationClass=char+0x448, Character=char,
//       CharStats=char+0x450).
//     SAFETY (verified in bytes): NOT idempotent (unconditional new(0x2F8) that overwrites
//       CharBody+0x8) → MANDATORY GUARD: only call when +0x8==NULL, otherwise the previous CombatClass
//       leaks. It only writes inside CharBody and the new CombatClass; no global registration. The
//       CombatClass comes out ready. Runs on the LOGIC THREAD (OnGameTick), never from the network
//       callback (the game 'new' allocator is not thread-safe).
//     It was put behind a toggle, originally OFF by default until [DIAG-ATTACK] confirmed the symptom.

// Toggle maestro del FIX-COMBATCLASS. ACTIVADO (2026-06-19): el [DIAG-ATTACK] ya confirmó en
// bytes el patrón de CausaA (CombatClass NULL en el host). La salvaguarda sigue intacta:
// SEH_FixCombatClass SOLO llama a CharBody::create si detecta *(CharBody+0x8)==NULL en el host
// (out->combatPre==0); si la CombatClass ya es válida marca alreadyHad y NO toca nada (idempotente,
// evita leak). Así que activarlo es seguro aunque algún char ya la tenga.
// EN: Master toggle of FIX-COMBATCLASS. ENABLED (2026-06-19): [DIAG-ATTACK] confirmed in bytes the
//     CauseA pattern (NULL CombatClass on the host). The safeguard is intact: SEH_FixCombatClass ONLY
//     calls CharBody::create if it sees *(CharBody+0x8)==NULL on the host (out->combatPre==0); if the
//     CombatClass is already valid it sets alreadyHad and touches nothing (idempotent, no leak).
//     NOTE (inconsistency): core.h and the inline comment in FixHostCombatClassTick still say "OFF by
//     default", but the value here is true.
// ES: NOTA (incoherencia): core.h y el comentario en línea de FixHostCombatClassTick siguen diciendo
//     "OFF por defecto", pero aquí el valor es true.
static constexpr bool kEnableCombatClassFix = true;

// Firma de CharBody::create (0x621460). __fastcall: 4 regs + 2 args en pila.
// EN: Signature of CharBody::create (0x621460). __fastcall: 4 registers + 2 stack args.
using CharBodyCreateFn = void(__fastcall*)(void* charBody, void* charMovement, void* ai,
                                           void* animClass, void* character, void* charStats);

// Estado one-shot del FIX-COMBATCLASS (re-armable en disconnect / nueva carga).
// EN: FIX-COMBATCLASS one-shot state (re-armable on disconnect / new load).
static std::atomic<bool> s_combatClassFixDone{false};
static int  s_combatClassAttempts = 0;
static auto s_lastCombatClassTry  = std::chrono::steady_clock::now();
static constexpr int COMBATCLASS_MAX_ATTEMPTS = 240; // ~varios segundos de reintentos (≈throttle 250ms)

// Re-arma el FIX-COMBATCLASS para una partida/conexión nueva (mismo patrón que FIX-CONTROL).
// EN: Re-arms FIX-COMBATCLASS for a new game/connection (same pattern as FIX-CONTROL).
void ResetHostCombatClassFix() {
    s_combatClassFixDone.store(false);
    s_combatClassAttempts = 0;
    s_lastCombatClassTry = std::chrono::steady_clock::now();
}

// Snapshot POD del FIX-COMBATCLASS (vive dentro del __try; sin objetos C++ → evita C2712).
// EN: FIX-COMBATCLASS POD snapshot (lives inside __try; no C++ objects → avoids C2712): CharBody
//     (char+0x648) and whether its vtable matches 0x16F8A68, CombatClass before/after (and vtable
//     0x16F67B8 check), the 5 CharBody::create args (movement, AI, anim, stats) and call status.
struct CombatClassFixSnapshot {
    uintptr_t charBody       = 0;  // char+0x648
    int       charBodyOk     = 0;  // 1 si CharBody válido y vtable == 0x16F8A68
    uintptr_t combatPre      = 0;  // *(CharBody+0x8) ANTES del fix (0 = falta → aplicar)
    uintptr_t combatPost     = 0;  // *(CharBody+0x8) DESPUÉS del fix
    int       combatPostOk   = 0;  // 1 si combatPost válido y vtable == 0x16F67B8
    // Los 5 args que necesita CharBody::create (todos deben ser válidos para llamar).
    uintptr_t charMovement   = 0;  // char+0x640
    uintptr_t ai             = 0;  // char+0x650
    uintptr_t animClass      = 0;  // char+0x448
    uintptr_t charStats      = 0;  // char+0x450
    int       argsOk         = 0;  // 1 si los 5 args son ptrs de heap válidos
    int       called         = 0;  // 1 llamado OK · -1 excepción dentro de la llamada nativa
    int       alreadyHad     = 0;  // 1 si CombatClass ya existía (idempotencia: no se llama)
};

// Lee el cluster, valida y (si procede) llama a CharBody::create. TODO bajo SEH.
// fn = CharBody::create (0x621460). combatVtblAbs/bodyVtblAbs = vtables absolutas para validar.
// EN: Reads the cluster, validates and (if appropriate) calls CharBody::create. ALL under SEH.
//     fn = CharBody::create (0x621460). combatVtblAbs/bodyVtblAbs = absolute vtables to validate.
static void SEH_FixCombatClass(uintptr_t chr, CharBodyCreateFn fn,
                               uintptr_t bodyVtblAbs, uintptr_t combatVtblAbs,
                               CombatClassFixSnapshot* out) {
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        return true;
    };
    __try {
        if (!isHeap(chr)) return;
        uintptr_t vtbl = 0;

        // CharBody (char+0x648) — debe existir y tener su vtable conocida.
        // EN: CharBody (char+0x648) — must exist and have its known vtable.
        uintptr_t body = 0;
        if (!Memory::Read(chr + 0x648, body) || !isHeap(body)) return;
        out->charBody = body;
        if (Memory::Read(body, vtbl) && vtbl == bodyVtblAbs) out->charBodyOk = 1;

        // CombatClass = *(CharBody+0x8). Si YA existe → idempotencia: NO llamar (evita leak).
        // EN: CombatClass = *(CharBody+0x8). If it ALREADY exists → idempotency: do NOT call (avoids leak).
        uintptr_t combat = 0;
        if (Memory::Read(body + 0x8, combat)) {
            out->combatPre = combat;
            if (combat != 0) { out->alreadyHad = 1; out->combatPost = combat; return; }
        }

        // Los 5 args de CharBody::create. Todos deben ser ptrs de heap válidos.
        // EN: The 5 args of CharBody::create. All must be valid heap pointers.
        uintptr_t mv = 0, ai = 0, an = 0, st = 0;
        Memory::Read(chr + 0x640, mv);  out->charMovement = mv;
        Memory::Read(chr + 0x650, ai);  out->ai           = ai;
        Memory::Read(chr + 0x448, an);  out->animClass    = an;
        Memory::Read(chr + 0x450, st);  out->charStats    = st;
        // Character (rcx-arg #5) es el propio char. CharStats puede ser NULL en algún char raro,
        // pero CharMovement/AI/AnimationClass son obligatorios (sin ellos el char no caminaría).
        // EN: Character (arg #5) is the char itself. CharStats may be NULL on some odd char, but
        //     CharMovement/AI/AnimationClass are mandatory (without them the char would not walk).
        if (!isHeap(mv) || !isHeap(ai) || !isHeap(an)) return;
        out->argsOk = 1;

        // Solo llegamos aquí si: CharBody válido + CombatClass==NULL + args válidos.
        // Llamada nativa a CharBody::create — RELLENA CharBody+0x8 con una CombatClass nueva.
        // EN: Only reached if: valid CharBody + CombatClass==NULL + valid args. Native call to
        //     CharBody::create — FILLS CharBody+0x8 with a new CombatClass.
        if (fn && out->charBodyOk && out->combatPre == 0) {
            fn(reinterpret_cast<void*>(body),        // rcx = CharBody (this)
               reinterpret_cast<void*>(mv),          // rdx = CharMovement
               reinterpret_cast<void*>(ai),          // r8  = AI
               reinterpret_cast<void*>(an),          // r9  = AnimationClass
               reinterpret_cast<void*>(chr),         // [rsp+0x28] = Character (el propio char)
               reinterpret_cast<void*>(st));         // [rsp+0x30] = CharStats
            out->called = 1;
            // Re-leer el resultado para el log antes/después.
            uintptr_t post = 0;
            if (Memory::Read(body + 0x8, post)) {
                out->combatPost = post;
                if (isHeap(post) && Memory::Read(post, vtbl))
                    out->combatPostOk = (vtbl == combatVtblAbs) ? 1 : 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->called = -1;
    }
}

// Orquesta el FIX-COMBATCLASS. Mismo patrón que SetHostControlledCharTick: throttle 250ms,
// one-shot re-armable, SOLO host, en el HILO DE LÓGICA (OnGameTick). Idempotente.
// EN: Drives FIX-COMBATCLASS. Same pattern as SetHostControlledCharTick: 250 ms throttle, re-armable
//     one-shot, host ONLY, on the LOGIC THREAD (OnGameTick). Idempotent.
static void FixHostCombatClassTick(Core& core) {
    if (!kEnableCombatClassFix) return;                         // toggle maestro (OFF por defecto)
    if (s_combatClassFixDone.load()) return;                    // ya resuelto (aplicado o ya tenía)
    if (s_combatClassAttempts >= COMBATCLASS_MAX_ATTEMPTS) return;

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastCombatClassTry < std::chrono::milliseconds(250)) return;
    s_lastCombatClassTry = now;
    s_combatClassAttempts++;

    uintptr_t modBase = Memory::GetModuleBase();
    if (modBase == 0) return;

    // char primario del host (mismo helper que usa el resto del mod / el [DIAG-ATTACK]).
    // EN: Host primary char (same helper the rest of the mod / [DIAG-ATTACK] use).
    uintptr_t hostChar = game::GetPlayerPrimaryCharacterDirect();
    if (hostChar <= 0x10000) return;                            // char aún no resoluble → reintentar

    // Vtables absolutas para validar CharBody/CombatClass (RVA confirmadas, audit-12).
    // Se usan literales aquí (CombatStructSnapshot se declara más abajo en el archivo).
    // EN: Absolute vtables to validate CharBody/CombatClass (RVAs confirmed, audit-12). Literals here
    //     (CombatStructSnapshot is declared further down in the file).
    // ES: RVA 0x621460 = CharBody::create dentro de kenshi_x64.exe (se LLAMA, no se hookea).
    // EN: RVA 0x621460 = CharBody::create inside kenshi_x64.exe (it is CALLED, not hooked).
    const uintptr_t bodyVtblAbs   = modBase + 0x16F8A68; // vtable CharBody    (char+0x648)
    const uintptr_t combatVtblAbs = modBase + 0x16F67B8; // vtable CombatClass (*(CharBody+0x8))
    CharBodyCreateFn fn = reinterpret_cast<CharBodyCreateFn>(modBase + 0x621460);

    CombatClassFixSnapshot fx{};
    SEH_FixCombatClass(hostChar, fn, bodyVtblAbs, combatVtblAbs, &fx);

    if (fx.alreadyHad) {
        // CombatClass ya existía → no era CausaA (o ya se aplicó): cerrar one-shot sin tocar nada.
        // EN: CombatClass already existed → it was not CauseA (or already applied): close the one-shot.
        s_combatClassFixDone.store(true);
        spdlog::info("[FIX-COMBATCLASS] CombatClass ya presente (*(CharBody+0x8)=0x{:X}) en host "
                     "char=0x{:X} → no se aplica (idempotente).", fx.combatPre, hostChar);
        return;
    }
    if (fx.called == 1 && fx.combatPost != 0) {
        s_combatClassFixDone.store(true);
        spdlog::info(
            "[FIX-COMBATCLASS] APLICADO ✓ char=0x{:X} CharBody=0x{:X}(ok={}) | "
            "CombatClass: 0x{:X} -> 0x{:X} (vtblOk={}) | args[move=0x{:X} ai=0x{:X} anim=0x{:X} "
            "stats=0x{:X}] (intento {}). El char ahora puede atacar (CombatClass poblada vía "
            "CharBody::create 0x621460).",
            hostChar, fx.charBody, fx.charBodyOk, fx.combatPre, fx.combatPost, fx.combatPostOk,
            fx.charMovement, fx.ai, fx.animClass, fx.charStats, s_combatClassAttempts);
        core.GetNativeHud().AddSystemMessage("Sistema de combate del jugador inicializado");
    } else if (fx.called == -1) {
        if (s_combatClassAttempts <= 5 || s_combatClassAttempts % 60 == 0) {
            spdlog::warn("[FIX-COMBATCLASS] excepción dentro de CharBody::create (char=0x{:X} "
                         "CharBody=0x{:X} args[move=0x{:X} ai=0x{:X} anim=0x{:X}]) — reintento {}.",
                         hostChar, fx.charBody, fx.charMovement, fx.ai, fx.animClass,
                         s_combatClassAttempts);
        }
    } else if (!fx.charBodyOk || !fx.argsOk) {
        // Char aún no tiene CharBody/subsistemas válidos → reintentar (sin spamear el log).
        // EN: The char does not have a valid CharBody/subsystems yet → retry (without spamming the log).
        if (s_combatClassAttempts <= 5 || s_combatClassAttempts % 60 == 0) {
            spdlog::info("[FIX-COMBATCLASS] aún no aplicable (char=0x{:X} CharBodyOk={} argsOk={} "
                         "combatPre=0x{:X}) — reintento {}.",
                         hostChar, fx.charBodyOk, fx.argsOk, fx.combatPre, s_combatClassAttempts);
        }
    } else if (s_combatClassAttempts == COMBATCLASS_MAX_ATTEMPTS) {
        spdlog::warn("[FIX-COMBATCLASS] sin completar tras {} intentos (called={} combatPre=0x{:X} "
                     "combatPost=0x{:X}). Revisar firma de CharBody::create 0x621460.",
                     s_combatClassAttempts, fx.called, fx.combatPre, fx.combatPost);
    }
}

// ══════════════════════════════════════════════════════════════════════════════════
// [FIX-COMBATARM] — Arranca la máquina de estados de combate del host (startupState)
// ══════════════════════════════════════════════════════════════════════════════════
// CAUSA (RE en vivo 2026-07-14, desensamblado + lecturas de memoria):
//   El host no puede iniciar combate "en frío" hasta que usa un muñeco de entrenamiento. El
//   campo nextMove (CombatClass+0x1F4, swordStateEnum) nace con BASURA sin inicializar (p.ej.
//   0xF9FA7C1D) porque la máquina de estados de combate del host NUNCA ejecutó su primer ciclo.
//   CombatClass::startupState() (vtable slot +0x50) es quien arranca esa máquina y deja
//   nextMove/combatState (CC+0x1F0) en valores válidos (enum pequeño 0..11, ver swordStateEnum).
//   El muñeco de entrenamiento fuerza ese primer ciclo de forma natural; el reclamo normal del
//   host (ClaimHostPrimaryCharacter) NO lo hace. Este fix lo fuerza por código UNA vez.
//
//   Cadena de resolución (misma que FIX-COMBATCLASS / [DIAG-ATTACK]):
//     CharBody   = *(host + 0x648)
//     CombatClass= *(CharBody + 0x8)          ; vtable esperada = modBase + 0x16F67B8
//     startupState = *(*(CombatClass) + 0x50) ; SIEMPRE por vtable, NUNCA por RVA fija
//       (las RVAs de esta zona de CombatClass NO son estables entre sesiones/deltas — verificado
//        esta noche; por eso se resuelve el puntero desde la vtable del propio objeto).
//   Firma confirmada (KenshiLib CombatClass.h): bool CombatClass::startupState();  // solo `this`.
//
// SEGURIDAD:
//   • Precondiciones (baratas) antes de llamar: CombatClass es heap válido, su vtable coincide con
//     la de CombatClass conocida (0x16F67B8), y `me` (CombatClass+0x188, back-ptr al Character) es
//     heap válido y == host (evita llamar sobre un objeto reusado/erróneo).
//   • RED DE SEGURIDAD: si tras la llamada (o si no se pudo llamar) nextMove (CC+0x1F4) sigue fuera
//     del rango razonable del enum (leído como uint32 > 0x40 → cubre basura y negativos), se escribe
//     nextMove=0 y combatState=0 (CHOP_WEAPON, valor de enum válido) como saneado defensivo.
//   • Thread safety: HILO DE LÓGICA (OnGameTick), igual que FIX-COMBATCLASS/FIX-PLATOON (NUNCA red).
//   • One-shot re-armable (ResetHostCombatArmFix), throttle 250ms, tope de intentos, todo bajo SEH.
//   • Corre DESPUÉS de FIX-COMBATCLASS (que crea la CombatClass si nació NULL): necesita que la
//     CombatClass ya exista para poder arrancar su máquina de estados.
// EN: [FIX-COMBATARM] — Boots the host combat state machine (startupState).
//     CAUSE (live RE 2026-07-14, disassembly + memory reads): the host cannot start combat "cold" until
//       it uses a training dummy. nextMove (CombatClass+0x1F4, swordStateEnum) is born with
//       UNINITIALIZED garbage (e.g. 0xF9FA7C1D) because the host combat state machine NEVER ran its
//       first cycle. CombatClass::startupState() (vtable slot +0x50) boots that machine and leaves
//       nextMove/combatState (CC+0x1F0) at valid values (small enum 0..11). The training dummy forces
//       that first cycle naturally; the normal host claim does not. This fix forces it ONCE.
//     Resolution chain (same as FIX-COMBATCLASS / [DIAG-ATTACK]): CharBody = *(host+0x648);
//       CombatClass = *(CharBody+0x8) (expected vtable = modBase+0x16F67B8); startupState =
//       *(*(CombatClass)+0x50) — ALWAYS through the vtable, NEVER a fixed RVA (RVAs in this area are
//       not stable across sessions/deltas). Signature (KenshiLib CombatClass.h): bool startupState().
//     SAFETY: cheap preconditions (valid heap CombatClass, known vtable, `me` back-pointer at
//       CombatClass+0x188 == host); SAFETY NET: if nextMove is still outside the enum range (uint32 >
//       0x40) nextMove=0 and combatState=0 (CHOP_WEAPON) are written; LOGIC THREAD only; re-armable
//       one-shot, 250 ms throttle, attempt cap, all under SEH; runs AFTER FIX-COMBATCLASS (needs the
//       CombatClass to exist).

// Toggle maestro del FIX-COMBATARM. DEFAULT TRUE.
// EN: Master toggle of FIX-COMBATARM. DEFAULT TRUE.
static constexpr bool kEnableCombatArmFix = true;

// Firma de CombatClass::startupState() (resuelta SIEMPRE por vtable slot +0x50, no por RVA).
//   rcx = CombatClass* (this). Devuelve bool. Sin más argumentos.
// EN: Signature of CombatClass::startupState() (ALWAYS resolved via vtable slot +0x50, not an RVA).
//     rcx = CombatClass* (this). Returns bool. No other args.
using CombatStartupStateFn = bool(__fastcall*)(void* combatClass);

// Estado one-shot del FIX-COMBATARM (re-armable en disconnect / nueva carga).
// EN: FIX-COMBATARM one-shot state (re-armable on disconnect / new load).
static std::atomic<bool> s_combatArmFixDone{false};
static int  s_combatArmAttempts = 0;
static auto s_lastCombatArmTry  = std::chrono::steady_clock::now();
static constexpr int COMBATARM_MAX_ATTEMPTS = 240; // ~varios segundos (≈throttle 250ms), como FIX-COMBATCLASS

// Re-arma el FIX-COMBATARM para una partida/conexión nueva (mismo patrón que FIX-COMBATCLASS).
// EN: Re-arms FIX-COMBATARM for a new game/connection (same pattern as FIX-COMBATCLASS).
void ResetHostCombatArmFix() {
    s_combatArmFixDone.store(false);
    s_combatArmAttempts = 0;
    s_lastCombatArmTry = std::chrono::steady_clock::now();
}

// Snapshot POD del FIX-COMBATARM (vive dentro del __try; sin objetos C++ → evita C2712).
// EN: FIX-COMBATARM POD snapshot (lives inside __try; no C++ objects → avoids C2712): CharBody,
//     CombatClass (+ vtable check), startupState pointer, `me` back-pointer, nextMove/combatState
//     before/after, and flags called/sanitized.
struct CombatArmFixSnapshot {
    uintptr_t charBody      = 0;  // host+0x648
    uintptr_t combat        = 0;  // *(CharBody+0x8) = CombatClass*
    int       combatOk      = 0;  // 1 si CombatClass válido y vtable == 0x16F67B8
    uintptr_t combatVtbl    = 0;  // *(CombatClass) (vtable) — de aquí sale startupState (+0x50)
    uintptr_t startupFn     = 0;  // *(vtable+0x50) — puntero a CombatClass::startupState
    uintptr_t me            = 0;  // CombatClass+0x188 (Character* back-ptr) — debe == host
    int       meOk          = 0;  // 1 si me es heap válido y == host
    uint32_t  nextMovePre   = 0;  // CC+0x1F4 ANTES (basura si la máquina no arrancó)
    uint32_t  combatStatePre= 0;  // CC+0x1F0 ANTES
    uint32_t  nextMovePost  = 0;  // CC+0x1F4 DESPUÉS de startupState / saneado
    uint32_t  combatStatePost=0;  // CC+0x1F0 DESPUÉS
    int       called        = 0;  // 1 startupState llamado OK · -1 excepción dentro de la llamada
    int       sanitized     = 0;  // 1 si la red de seguridad reescribió nextMove/combatState a 0
};

// Rango de código del JUEGO (kenshi_x64.exe): [inicio de .text, fin de .rdata). Un puntero de
// FUNCIÓN válido del juego (p.ej. CombatClass::startupState) SIEMPRE cae aquí: vive en la sección
// .text del módulo. Se usa para validar startupFn en SEH_FixCombatArm (mismo criterio que el guard
// UAF de BuyItem en inventory_hooks.cpp). Se calcula una vez recorriendo las secciones PE.
// EN: Game CODE range (kenshi_x64.exe): [start of .text, end of .rdata). A valid game FUNCTION pointer
//     (e.g. CombatClass::startupState) always lands here. Used to validate startupFn in
//     SEH_FixCombatArm (same criterion as the BuyItem UAF guard in inventory_hooks.cpp). Computed once
//     by walking the PE sections.
static uintptr_t s_combatArmGameTextLo = 0, s_combatArmGameRdataHi = 0;

// Calcula [.text, .rdata) del EXE del juego recorriendo sus secciones PE. Idempotente. Se llama
// FUERA del __try de SEH_FixCombatArm (no crea objetos C++ con destructor dentro del guard).
// EN: Computes [.text, .rdata) of the game EXE by walking its PE sections. Idempotent. Called OUTSIDE
//     the SEH_FixCombatArm __try (no C++ objects with destructors inside the guard).
static void EnsureCombatArmGameCodeRange() {
    if (s_combatArmGameTextLo && s_combatArmGameRdataHi) return;   // ya calculado antes
    HMODULE h = GetModuleHandleW(nullptr);      // el propio kenshi_x64.exe (no el mod inyectado)
    auto base = reinterpret_cast<uintptr_t>(h);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec  = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = {0}; memcpy(name, sec[i].Name, 8);       // nombre de sección (máx 8 chars)
        uintptr_t lo = base + sec[i].VirtualAddress;
        uintptr_t hi = lo + sec[i].Misc.VirtualSize;
        if (!strcmp(name, ".text"))  s_combatArmGameTextLo  = lo;   // inicio del código nativo
        if (!strcmp(name, ".rdata")) s_combatArmGameRdataHi = hi;   // fin de datos de solo lectura
    }
    // Fallbacks defensivos si el PE no expone esas secciones con esos nombres exactos.
    // EN: Defensive fallbacks if the PE does not expose those sections with those exact names.
    //     (base+0x1000 = usual start of .text; the .rdata end is a hard-coded value taken from the
    //     Steam 1.0.68 layout, probably wrong for other builds.)
    // ES: (base+0x1000 = inicio habitual de .text; el fin de .rdata es un valor fijo sacado del layout de
    //     Steam 1.0.68, probablemente incorrecto en otras builds.)
    if (!s_combatArmGameTextLo)  s_combatArmGameTextLo  = base + 0x1000;
    if (!s_combatArmGameRdataHi) s_combatArmGameRdataHi = base + 0x1673000 + 0x54A4CB;
}

// Lee el cluster de combate del host, resuelve startupState por vtable, lo llama (si procede) y
// aplica la red de seguridad. TODO bajo SEH. combatVtblAbs = vtable CombatClass absoluta (validar).
// EN: Reads the host combat cluster, resolves startupState through the vtable, calls it (if
//     appropriate) and applies the safety net. ALL under SEH. combatVtblAbs = absolute CombatClass
//     vtable (to validate).
static void SEH_FixCombatArm(uintptr_t chr, uintptr_t combatVtblAbs, CombatArmFixSnapshot* out) {
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;   // punteros del juego alineados a 8
        return true;
    };
    __try {
        if (!isHeap(chr)) return;

        // CharBody (host+0x648) — debe existir.
        // EN: CharBody (host+0x648) — must exist.
        uintptr_t body = 0;
        if (!Memory::Read(chr + 0x648, body) || !isHeap(body)) return;
        out->charBody = body;

        // CombatClass = *(CharBody+0x8). Si es NULL, aún no hay CombatClass (FIX-COMBATCLASS no
        // corrió todavía) → no aplicable: reintentar.
        // EN: CombatClass = *(CharBody+0x8). If NULL there is no CombatClass yet (FIX-COMBATCLASS has not run)
        //     → not applicable: retry.
        uintptr_t combat = 0;
        if (!Memory::Read(body + 0x8, combat) || !isHeap(combat)) return;
        out->combat = combat;

        // vtable del CombatClass — debe coincidir con la conocida (0x16F67B8). De ella sale
        // startupState en el slot +0x50 (resolución SIEMPRE por vtable, nunca por RVA fija).
        // EN: CombatClass vtable — must match the known one (0x16F67B8). startupState comes from its slot +0x50
        //     (ALWAYS resolved through the vtable, never a fixed RVA).
        uintptr_t vtbl = 0;
        if (!Memory::Read(combat, vtbl) || !isHeap(vtbl)) return;
        out->combatVtbl = vtbl;
        if (vtbl != combatVtblAbs) return;   // vtable inesperada → NO tocar (objeto no es CombatClass)
        out->combatOk = 1;

        // `me` (CombatClass+0x188) = Character* back-ptr. Debe ser heap válido y == host: garantiza
        // que este CombatClass pertenece REALMENTE al char del host (evita objeto reusado/erróneo).
        // EN: `me` (CombatClass+0x188) = Character* back-pointer. Must be valid heap and == host: guarantees this
        //     CombatClass REALLY belongs to the host char (avoids a reused/wrong object).
        uintptr_t me = 0;
        Memory::Read(combat + 0x188, me);
        out->me = me;
        if (!isHeap(me) || me != chr) return; // precondición barata fallida → no llamar startupState
        out->meOk = 1;

        // Estado ANTES (evidencia): nextMove (CC+0x1F4) y combatState (CC+0x1F0).
        // EN: State BEFORE (evidence): nextMove (CC+0x1F4) and combatState (CC+0x1F0).
        Memory::Read(combat + 0x1F4, out->nextMovePre);
        Memory::Read(combat + 0x1F0, out->combatStatePre);

        // Resolver startupState por la vtable del propio objeto: slot en byte-offset +0x50.
        // OJO: aquí validamos un puntero de FUNCIÓN, no de heap. isHeap() está diseñado para punteros
        // de HEAP y RECHAZA SIEMPRE cualquier dirección dentro del módulo del juego (por diseño:
        // detecta objetos que en realidad caen en código/vtable). Pero un entry point de función
        // VIVE en .text del módulo → isHeap(fnAddr) lo rechazaba SIEMPRE aunque fuese válido (bug
        // confirmado en vivo: Memory::Read devolvía un puntero legítimo y no-nulo, pero isHeap lo
        // tumbaba y startupFn quedaba en 0x0 → la máquina de estados nunca se armaba). Criterio
        // correcto (mismo que el guard UAF de BuyItem): fnAddr debe caer en el rango de código del
        // JUEGO [.text, .rdata). SIN exigir alineación a 8: un entry point de función no la garantiza.
        // EN: Resolve startupState from the object own vtable: slot at byte offset +0x50.
        //     NOTE: this validates a FUNCTION pointer, not a heap one. isHeap() is meant for HEAP pointers and
        //     ALWAYS rejects addresses inside the game module, so it used to reject a valid entry point (bug
        //     confirmed live: startupFn stayed 0x0 and the state machine never armed). Correct criterion (same
        //     as the BuyItem UAF guard): fnAddr must be inside the game code range [.text, .rdata), WITHOUT
        //     requiring 8-byte alignment (function entry points do not guarantee it).
        uintptr_t fnAddr = 0;
        if (Memory::Read(vtbl + 0x50, fnAddr) &&
            fnAddr >= s_combatArmGameTextLo && fnAddr < s_combatArmGameRdataHi) {
            out->startupFn = fnAddr;
            CombatStartupStateFn fn = reinterpret_cast<CombatStartupStateFn>(fnAddr);
            // Llamada nativa UNA vez: arranca la máquina de estados de combate del host.
            // EN: Native call ONCE: boots the host combat state machine.
            fn(reinterpret_cast<void*>(combat));   // rcx = CombatClass (this)
            out->called = 1;
        }

        // Re-leer el resultado tras la llamada (o sin llamada si startupFn no era válido).
        // EN: Re-read the result after the call (or without a call if startupFn was not valid).
        Memory::Read(combat + 0x1F4, out->nextMovePost);
        Memory::Read(combat + 0x1F0, out->combatStatePost);

        // RED DE SEGURIDAD: si nextMove sigue fuera del rango del enum (uint32 > 0x40 → cubre basura
        // tipo 0xF9FA7C1D y negativos interpretados como grandes), sanear a 0 (CHOP_WEAPON, válido).
        // EN: SAFETY NET: if nextMove is still outside the enum range (uint32 > 0x40 → covers garbage like
        //     0xF9FA7C1D and negatives read as large values), sanitize to 0 (CHOP_WEAPON, valid).
        if (out->nextMovePost > 0x40) {
            uint32_t zero = 0;
            Memory::Write(combat + 0x1F4, zero);   // nextMove   = 0
            Memory::Write(combat + 0x1F0, zero);   // combatState= 0
            out->sanitized = 1;
            // Reflejar el saneado en el snapshot para el log.
            Memory::Read(combat + 0x1F4, out->nextMovePost);
            Memory::Read(combat + 0x1F0, out->combatStatePost);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->called = -1;
    }
}

// Orquesta el FIX-COMBATARM. Mismo patrón que FixHostCombatClassTick: throttle 250ms, one-shot
// re-armable, SOLO host, en el HILO DE LÓGICA (OnGameTick). Idempotente (one-shot).
// EN: Drives FIX-COMBATARM. Same pattern as FixHostCombatClassTick: 250 ms throttle, re-armable
//     one-shot, host ONLY, on the LOGIC THREAD (OnGameTick). Idempotent (one-shot).
static void FixHostCombatArmTick(Core& core) {
    if (!kEnableCombatArmFix) return;                           // toggle maestro
    if (s_combatArmFixDone.load()) return;                      // ya resuelto (arrancado/saneado)
    if (s_combatArmAttempts >= COMBATARM_MAX_ATTEMPTS) return;

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastCombatArmTry < std::chrono::milliseconds(250)) return;
    s_lastCombatArmTry = now;

    uintptr_t modBase = Memory::GetModuleBase();
    if (modBase == 0) return;

    // Rango de código del juego [.text,.rdata) para validar el puntero de función startupState dentro
    // de SEH_FixCombatArm. Se calcula aquí (fuera del __try) porque recorre las secciones PE una vez.
    // EN: Game code range [.text,.rdata) to validate the startupState function pointer inside
    //     SEH_FixCombatArm. Computed here (outside the __try) because it walks the PE sections once.
    EnsureCombatArmGameCodeRange();

    // char primario del host (mismo helper que el resto del mod / FIX-COMBATCLASS / [DIAG-ATTACK]).
    // EN: Host primary char (same helper as the rest of the mod / FIX-COMBATCLASS / [DIAG-ATTACK]).
    uintptr_t hostChar = game::GetPlayerPrimaryCharacterDirect();
    if (hostChar <= 0x10000) return;                            // host aún no resoluble → reintentar

    // A partir de aquí HAY host real → intento REAL: contarlo.
    // EN: From here on there IS a real host → REAL attempt: count it.
    s_combatArmAttempts++;

    const uintptr_t combatVtblAbs = modBase + 0x16F67B8;        // vtable CombatClass (*(CharBody+0x8))

    CombatArmFixSnapshot fx{};
    SEH_FixCombatArm(hostChar, combatVtblAbs, &fx);

    // RVAs relativos para el log (independientes del ASLR).
    // EN: Relative RVAs for the log (ASLR-independent).
    auto rva = [modBase](uintptr_t a) -> uintptr_t { return (a > modBase) ? (a - modBase) : 0; };

    if (fx.called == 1) {
        // startupState se ejecutó (con o sin saneado posterior): cerrar one-shot.
        // EN: startupState ran (with or without later sanitizing): close the one-shot.
        s_combatArmFixDone.store(true);
        spdlog::info(
            "[FIX-COMBATARM] APLICADO ✓ char=0x{:X} CombatClass=0x{:X}(vtblOk={}) me=0x{:X}(ok={}) | "
            "startupState=vtbl+0x50 (fn=0x{:X} rva=0x{:X}) | nextMove(CC+0x1F4): 0x{:X} -> 0x{:X} | "
            "combatState(CC+0x1F0): 0x{:X} -> 0x{:X} | saneado={} (intento {}). La máquina de estados "
            "de combate del host queda arrancada → puede iniciar combate en frío sin muñeco.",
            hostChar, fx.combat, fx.combatOk, fx.me, fx.meOk, fx.startupFn, rva(fx.startupFn),
            fx.nextMovePre, fx.nextMovePost, fx.combatStatePre, fx.combatStatePost, fx.sanitized,
            s_combatArmAttempts);
        core.GetNativeHud().AddSystemMessage("Sistema de combate del jugador armado");
    } else if (fx.sanitized == 1) {
        // No se pudo llamar startupState (fn no resuelto) PERO la red de seguridad saneó la basura:
        // también cerramos el one-shot (nextMove/combatState quedan en un valor válido).
        // EN: startupState could NOT be called (fn unresolved) BUT the safety net sanitized the garbage: close
        //     the one-shot too (nextMove/combatState end at a valid value).
        s_combatArmFixDone.store(true);
        spdlog::warn(
            "[FIX-COMBATARM] startupState NO llamado (fn=0x{:X}) pero RED DE SEGURIDAD aplicada: "
            "nextMove(CC+0x1F4) 0x{:X} -> 0x{:X}, combatState(CC+0x1F0) 0x{:X} -> 0x{:X} en char=0x{:X} "
            "(intento {}).",
            fx.startupFn, fx.nextMovePre, fx.nextMovePost, fx.combatStatePre, fx.combatStatePost,
            hostChar, s_combatArmAttempts);
    } else if (fx.called == -1) {
        if (s_combatArmAttempts <= 5 || s_combatArmAttempts % 60 == 0) {
            spdlog::warn("[FIX-COMBATARM] excepción dentro de startupState/saneado (char=0x{:X} "
                         "CombatClass=0x{:X} vtbl=0x{:X} fn=0x{:X}) — reintento {}.",
                         hostChar, fx.combat, fx.combatVtbl, fx.startupFn, s_combatArmAttempts);
        }
    } else {
        // Aún no aplicable (sin CombatClass, vtable inesperada, o `me` no coincide) → reintentar.
        // EN: Not applicable yet (no CombatClass, unexpected vtable, or `me` mismatch) → retry.
        if (s_combatArmAttempts <= 5 || s_combatArmAttempts % 60 == 0) {
            spdlog::info("[FIX-COMBATARM] aún no aplicable (char=0x{:X} CombatClass=0x{:X} combatOk={} "
                         "meOk={} nextMovePre=0x{:X}) — reintento {} (esperando CombatClass del host).",
                         hostChar, fx.combat, fx.combatOk, fx.meOk, fx.nextMovePre, s_combatArmAttempts);
        }
        if (s_combatArmAttempts == COMBATARM_MAX_ATTEMPTS) {
            spdlog::warn("[FIX-COMBATARM] sin completar tras {} intentos (combatOk={} meOk={} "
                         "called={}). Revisar resolución vtable+0x50 de startupState.",
                         s_combatArmAttempts, fx.combatOk, fx.meOk, fx.called);
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════════════
// [FIX-ARMHAND] — Repara el handle de brazo del AI del host (AI+0x318 → Character+0x458)
// ══════════════════════════════════════════════════════════════════════════════════
// CAUSA (RE estático de alta confianza, 2026-07-14, desensamblado de la cadena nativa
//   giveBirth→createComponents→AI::create, RVAs 0x62b210 / 0x62af50 / 0x622110):
//   El host no puede CARGAR/SECUESTRAR personas, hacer PRIMEROS AUXILIOS a otros ni
//   AUTOCURARSE: el chequeo GOAP nativo devuelve "brazo en estado pésimo" AUNQUE los brazos
//   estén sanos (100/100). El motivo NO es la salud del brazo (eso ya lo cubre el [FIX-ARMGATE]
//   / [FIX-ARMSEED]) sino que el handle en AI+0x318 no apunta a donde debería tras el reclamo
//   del host por el mod (el spawn de la plantilla del mod no reprodujo esta parte de AI::create).
//
//   En un personaje SANO, AI+0x318 debe apuntar a Character+0x458 — una región que YA EXISTE
//   dentro del propio objeto Character desde que nace. Por tanto NO hay que alocar memoria nueva
//   ni llamar a ningún constructor: es una simple ESCRITURA DE PUNTERO de 8 bytes.
//     AI      = *(Character + 0x650)          ; puntero al AI del char
//     destino = AI + 0x318                    ; handle a reparar
//     valor   = Character + 0x458             ; dirección DENTRO del propio Character
//   Offsets confirmados ESTABLES (no cambian entre Steam/GOG/versión): AI+0x318, Character+0x458,
//   Character+0x650.
//
// SEGURIDAD:
//   • Resuelve el char del host con game::GetPlayerPrimaryCharacterDirect() (= data[0] del
//     PlayerInterface, mismo helper que FIX-COMBATCLASS/FIX-COMBATARM). Esto GARANTIZA que 'chr'
//     es una base Character REAL, así que Character+0x458 existe y la escritura no puede caer
//     sobre un objeto que no sea Character (evita corrupción). Por eso este fix se limita al host
//     (mismo alcance que los fixes hermanos de escritura cruda COMBATCLASS/COMBATARM; el
//     SeedHostCharArmFlagsTick que sí itera varios chars usa una LLAMADA NATIVA de reevaluación,
//     no una escritura de puntero, así que no es precedente para ampliar el alcance aquí).
//   • Auto-verificación barata antes de escribir: AI = *(chr+0x650) debe ser un puntero de HEAP
//     válido (isHeap). Se lee el valor ACTUAL de [AI+0x318] para loguear antes/después (útil para
//     diagnóstico) y para no reescribir si ya estaba correcto (idempotente).
//   • Thread safety: HILO DE LÓGICA (OnGameTick), igual que FIX-COMBATCLASS/FIX-COMBATARM (NUNCA red).
//   • One-shot re-armable (ResetHostArmHandFix), throttle 250ms, tope de intentos, todo bajo SEH.
//   • Corre DESPUÉS de FIX-COMBATCLASS/FIX-COMBATARM (que dependen de que host/AI estén resueltos):
//     este fix también asume el AI (char+0x650) ya materializado.
// EN: [FIX-ARMHAND] — Repairs the host AI arm handle (AI+0x318 → Character+0x458).
//     CAUSE (high-confidence static RE, 2026-07-14, native chain giveBirth→createComponents→AI::create,
//       RVAs 0x62b210 / 0x62af50 / 0x622110): the host cannot CARRY/KIDNAP people, give FIRST AID to
//       others nor HEAL ITSELF: the native GOAP check returns "arm in terrible condition" EVEN with
//       healthy arms (100/100). The reason is NOT arm health (covered by [FIX-ARMGATE]/[FIX-ARMSEED])
//       but that the handle at AI+0x318 does not point where it should after the mod claims the host
//       (the mod template spawn did not reproduce this part of AI::create).
//       In a HEALTHY char, AI+0x318 must point to Character+0x458 — a region that ALREADY EXISTS inside
//       the Character object since birth. So no allocation nor constructor call is needed: it is a plain
//       8-byte POINTER WRITE (AI = *(Character+0x650); target = AI+0x318; value = Character+0x458).
//       Offsets confirmed STABLE across Steam/GOG/version: AI+0x318, Character+0x458, Character+0x650.
//     SAFETY: the host char comes from game::GetPlayerPrimaryCharacterDirect() (data[0] of the
//       PlayerInterface), which GUARANTEES a REAL Character base, so the write cannot land on another
//       object type; hence the fix is host-only. Cheap self-check before writing (AI must be a valid heap
//       pointer); the CURRENT [AI+0x318] is read for before/after logging and to skip rewriting if
//       already correct (idempotent). LOGIC THREAD only. Re-armable one-shot (ResetHostArmHandFix),
//       250 ms throttle, attempt cap, all under SEH. Runs AFTER FIX-COMBATCLASS/FIX-COMBATARM.

// Toggle maestro del FIX-ARMHAND. DEFAULT TRUE (escritura idempotente de bajo riesgo).
// EN: Master toggle of FIX-ARMHAND. DEFAULT TRUE (idempotent low-risk write).
static constexpr bool kEnableArmHandFix = true;

// Estado one-shot del FIX-ARMHAND (re-armable en disconnect / nueva carga).
// EN: FIX-ARMHAND one-shot state (re-armable on disconnect / new load).
static std::atomic<bool> s_armHandFixDone{false};
static int  s_armHandAttempts = 0;
static auto s_lastArmHandTry  = std::chrono::steady_clock::now();
static constexpr int ARMHAND_MAX_ATTEMPTS = 240; // ~varios segundos (≈throttle 250ms), como FIX-COMBATARM

// Re-arma el FIX-ARMHAND para una partida/conexión nueva (mismo patrón que FIX-COMBATARM).
// EN: Re-arms FIX-ARMHAND for a new game/connection (same pattern as FIX-COMBATARM).
void ResetHostArmHandFix() {
    s_armHandFixDone.store(false);
    s_armHandAttempts = 0;
    s_lastArmHandTry = std::chrono::steady_clock::now();
}

// Snapshot POD del FIX-ARMHAND (vive dentro del __try; sin objetos C++ → evita C2712).
// EN: FIX-ARMHAND POD snapshot (lives inside __try; no C++ objects → avoids C2712): host char, AI
//     pointer, expected value (chr+0x458), handle before/after and flags alreadyOk/repaired/excepted.
struct ArmHandFixSnapshot {
    uintptr_t chr        = 0;  // char del host (base Character real)
    uintptr_t ai         = 0;  // *(chr+0x650) = puntero al AI
    int       aiOk       = 0;  // 1 si ai es heap válido
    uintptr_t expected   = 0;  // chr+0x458 (valor que debe quedar en AI+0x318)
    uintptr_t handlePre  = 0;  // [ai+0x318] ANTES (evidencia: apunta mal si el bug está presente)
    uintptr_t handlePost = 0;  // [ai+0x318] DESPUÉS de la escritura
    int       alreadyOk  = 0;  // 1 si [ai+0x318] ya == chr+0x458 (nada que hacer)
    int       repaired   = 0;  // 1 si se escribió el puntero
    int       excepted   = 0;  // -1 si saltó excepción dentro del guard
};

// Lee AI = *(chr+0x650), valida, lee [AI+0x318] y escribe chr+0x458 si procede. TODO bajo SEH.
// EN: Reads AI = *(chr+0x650), validates it, reads [AI+0x318] and writes chr+0x458 if needed. ALL under
//     SEH.
static void SEH_FixArmHand(uintptr_t chr, ArmHandFixSnapshot* out) {
    // Criterio de puntero de HEAP válido (mismo que SEH_FixCombatArm): no-nulo, dentro del rango
    // canónico de usuario y alineado a 8 (los punteros del juego lo están).
    // EN: Valid HEAP pointer criterion (same as SEH_FixCombatArm): non-null, inside the canonical user range
    //     and 8-byte aligned (game pointers are).
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        return true;
    };
    __try {
        if (!isHeap(chr)) return;
        out->chr = chr;

        // AI = *(Character+0x650). Si aún no está materializado (NULL/no-heap) → no aplicable: reintentar.
        // EN: AI = *(Character+0x650). If not materialized yet (NULL/non-heap) → not applicable: retry.
        uintptr_t ai = 0;
        if (!Memory::Read(chr + 0x650, ai) || !isHeap(ai)) return;
        out->ai = ai;
        out->aiOk = 1;

        // Valor que debe quedar en AI+0x318: la región Character+0x458 DENTRO del propio char.
        // EN: Value that must end up at AI+0x318: the Character+0x458 region INSIDE the char itself.
        uintptr_t expected = chr + 0x458;
        out->expected = expected;

        // Valor ACTUAL del handle (evidencia antes/después; también sirve para la idempotencia).
        // EN: CURRENT handle value (before/after evidence; also used for idempotency).
        Memory::Read(ai + 0x318, out->handlePre);

        if (out->handlePre == expected) {
            // Ya está reparado (idempotente): no reescribir.
            // EN: Already repaired (idempotent): do not rewrite.
            out->alreadyOk  = 1;
            out->handlePost = out->handlePre;
            return;
        }

        // Escritura de puntero de 8 bytes: AI+0x318 = Character+0x458.
        // EN: 8-byte pointer write: AI+0x318 = Character+0x458.
        if (Memory::Write(ai + 0x318, expected)) {
            out->repaired = 1;
        }
        // Releer para confirmar/loguear el resultado.
        // EN: Re-read to confirm/log the result.
        Memory::Read(ai + 0x318, out->handlePost);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->excepted = -1;
    }
}

// Orquesta el FIX-ARMHAND. Mismo patrón que FixHostCombatArmTick: throttle 250ms, one-shot
// re-armable, SOLO host, en el HILO DE LÓGICA (OnGameTick). Idempotente (one-shot).
// EN: Drives FIX-ARMHAND. Same pattern as FixHostCombatArmTick: 250 ms throttle, re-armable one-shot,
//     host ONLY, on the LOGIC THREAD (OnGameTick). Idempotent (one-shot).
static void FixHostArmHandTick(Core& core) {
    if (!kEnableArmHandFix) return;                            // toggle maestro
    if (s_armHandFixDone.load()) return;                       // ya resuelto (reparado o ya correcto)
    if (s_armHandAttempts >= ARMHAND_MAX_ATTEMPTS) return;

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastArmHandTry < std::chrono::milliseconds(250)) return;
    s_lastArmHandTry = now;

    // char primario del host (mismo helper que FIX-COMBATCLASS/FIX-COMBATARM): base Character REAL.
    // EN: Host primary char (same helper as FIX-COMBATCLASS/FIX-COMBATARM): REAL Character base.
    uintptr_t hostChar = game::GetPlayerPrimaryCharacterDirect();
    if (hostChar <= 0x10000) return;                           // host aún no resoluble → reintentar

    // A partir de aquí HAY host real → intento REAL: contarlo.
    // EN: From here on there IS a real host → REAL attempt: count it.
    s_armHandAttempts++;

    ArmHandFixSnapshot fx{};
    SEH_FixArmHand(hostChar, &fx);

    // (chr/ai son punteros de HEAP, no del módulo → se loguean en absoluto, sin convertir a RVA).
    // EN: (chr/ai are HEAP pointers, not module ones → logged as absolute addresses, not RVAs.)
    if (fx.repaired == 1) {
        // Se reparó el handle: cerrar one-shot.
        // EN: The handle was repaired: close the one-shot.
        s_armHandFixDone.store(true);
        spdlog::info(
            "[FIX-ARMHAND] AI+0x318 reparado: 0x{:X} -> 0x{:X} (char=0x{:X} Character+0x458=0x{:X} "
            "AI=0x{:X}) (intento {}). El host ya puede cargar/secuestrar, hacer primeros auxilios y "
            "autocurarse (el chequeo GOAP de brazo deja de fallar).",
            fx.handlePre, fx.handlePost, fx.chr, fx.expected, fx.ai, s_armHandAttempts);
        core.GetNativeHud().AddSystemMessage("Brazo del jugador reparado (cargar/curar habilitado)");
    } else if (fx.alreadyOk == 1) {
        // Ya estaba correcto (idempotente): cerrar one-shot sin escribir.
        // EN: Already correct (idempotent): close the one-shot without writing.
        s_armHandFixDone.store(true);
        spdlog::info(
            "[FIX-ARMHAND] AI+0x318 ya correcto (0x{:X} == Character+0x458) en char=0x{:X} (AI=0x{:X}) "
            "— nada que reparar (intento {}).",
            fx.handlePre, fx.chr, fx.ai, s_armHandAttempts);
    } else if (fx.excepted == -1) {
        if (s_armHandAttempts <= 5 || s_armHandAttempts % 60 == 0) {
            spdlog::warn("[FIX-ARMHAND] excepción dentro de la reparación (char=0x{:X} AI=0x{:X} "
                         "aiOk={}) — reintento {}.",
                         fx.chr, fx.ai, fx.aiOk, s_armHandAttempts);
        }
    } else {
        // Aún no aplicable (AI no materializado, o Write falló) → reintentar.
        // EN: Not applicable yet (AI not materialized, or Write failed) → retry.
        if (s_armHandAttempts <= 5 || s_armHandAttempts % 60 == 0) {
            spdlog::info("[FIX-ARMHAND] aún no aplicable (char=0x{:X} AI=0x{:X} aiOk={} "
                         "handlePre=0x{:X}) — reintento {} (esperando AI del host en char+0x650).",
                         fx.chr, fx.ai, fx.aiOk, fx.handlePre, s_armHandAttempts);
        }
        if (s_armHandAttempts == ARMHAND_MAX_ATTEMPTS) {
            spdlog::warn("[FIX-ARMHAND] sin completar tras {} intentos (aiOk={} handlePre=0x{:X}). "
                         "Revisar resolución AI=*(char+0x650) y offset AI+0x318.",
                         s_armHandAttempts, fx.aiOk, fx.handlePre);
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════════════
// [FIX-PLATOON] — Re-enlaza el ActivePlatoon del host con setActivePlatoon (Fase 4)
// ══════════════════════════════════════════════════════════════════════════════════
// CAUSA RAÍZ (confirmada en runtime + RE de bytes, Steam 1.0.68):
//   El spawn de la plantilla del mod (clon 'Player 1') copia el ActivePlatoon (char+0x658,
//   no-NULL → pasa el gate 0x5C6650) PERO no llama a Character::setActivePlatoon (0x6213F0),
//   que es quien hace el REGISTRO AI<->platoon. Resultado: la orden de combate del host SÍ se
//   encola en el Tasker del platoon (char+0x658→+0x98, log [DIAG-PUSHORDER]), pero el AI tick
//   NUNCA la consume porque AI+0x10 (el puntero al Platoon* que el AI tick usa para localizar
//   su platoon) quedó en NULL/basura → el Tasker queda HUÉRFANO → amIdle=1 para siempre.
//
// QUÉ HACE setActivePlatoon 0x6213F0 (RE de bytes CONFIRMADO, 16 instrucciones, función COMPLETA):
//   void __fastcall(Character* rcx, void* activePlatoon /*rdx*/, int memberIdx /*r8d*/)
//     +0x0D  mov [char+0x658], activePlatoon          ; escribe el ActivePlatoon* en el char
//     +0x14  mov rcx, [char+0x650]                    ; rcx = AI*
//     +0x1B  mov edi, r8d                             ; guarda el índice de miembro
//     +0x1E  test rdx,rdx / je                        ; si platoon==NULL → registro con rdx=0
//     +0x23  mov rdx, [platoon+0x78]                  ; rdx = platoon->me (= Platoon*)
//     +0x27  call 0x14000CAD1  → thunk → 0x506CC0 = { mov [AI+0x10], rdx; ret }
//                                                     ; *** REGISTRO AI+0x10 = Platoon* ***
//     +0x2C  if (char+0x658 != 0)  mov [char+0x418], memberIdx  ; índice del char en el platoon
//   ⚠ CONFIRMADO: NO hay early-return si el platoon es el mismo (escribe y registra SIEMPRE),
//     NO hace AddRef/Release (no toca refcounts). → re-llamarla con el platoon que YA tiene es
//     IDEMPOTENTE y SEGURO: re-escribe punteros idénticos y re-ejecuta el registro AI+0x10 omitido.
//
// OPCIÓN ELEGIDA = (a): re-llamar setActivePlatoon(host, *(host+0x658), *(host+0x418)) con el
//   ActivePlatoon que el char YA tiene en char+0x658 y su índice de miembro actual (char+0x418),
//   para forzar el registro AI<->platoon que el spawn del mod omitió, SIN inventar valores nuevos.
//   No usamos (b) (el del squadleader) porque el char ya tiene un ActivePlatoon válido (pasa el
//   gate 0x5C6650 y el [DIAG-PUSHORDER] confirma que su Tasker recibe la orden); solo falta el
//   registro. Re-pasar el memberIdx actual hace que la escritura [char+0x418] no cambie nada.
//
// SEGURIDAD / THREAD: se ejecuta en el HILO DE LÓGICA (OnGameTick), igual que FIX-CONTROL/
// FIX-COMBATCLASS (NUNCA desde el callback de red — la red no debe mutar estructuras del motor).
// Toda lectura y la llamada nativa van bajo SEH en un helper POD (sin objetos C++ → evita C2712).
// One-shot re-armable en disconnect/nueva carga (ResetHostPlatoonFix). Toggle kEnablePlatoonFix
// DEFAULT true. Throttle 250ms, máx. de intentos acotado. Corre ANTES del [AUTOTEST] para que el
// ciclo de prueba mida el efecto (amIdle→0 + Task_MeleeAttack al atacar).
// EN: [FIX-PLATOON] — Re-links the host ActivePlatoon with setActivePlatoon (Phase 4).
//     ROOT CAUSE (confirmed at runtime + byte RE, Steam 1.0.68): the mod template spawn (clone
//       'Player 1') copies the ActivePlatoon (char+0x658, non-NULL → passes gate 0x5C6650) BUT does not
//       call Character::setActivePlatoon (0x6213F0), which performs the AI<->platoon REGISTRATION. So the
//       host combat order IS queued in the platoon Tasker (char+0x658→+0x98, [DIAG-PUSHORDER] log) but
//       the AI tick NEVER consumes it because AI+0x10 (the Platoon* the AI tick uses to find its
//       platoon) is NULL/garbage → the Tasker is ORPHANED → amIdle=1 forever.
//     WHAT setActivePlatoon 0x6213F0 DOES (byte RE CONFIRMED, whole function, see listing above):
//       void __fastcall(Character* rcx, void* activePlatoon, int memberIdx): writes char+0x658 =
//       activePlatoon; takes AI = char+0x650; if platoon != NULL loads platoon->me ([platoon+0x78],
//       a Platoon*) and calls a thunk that does AI+0x10 = Platoon* (THE REGISTRATION); if char+0x658 !=
//       0 writes char+0x418 = memberIdx. No early return for the same platoon, no AddRef/Release → calling
//       it again with the platoon it ALREADY has is IDEMPOTENT and SAFE.
//     CHOSEN OPTION (a): call setActivePlatoon(host, *(host+0x658), *(host+0x418)) with the char current
//       ActivePlatoon and member index, forcing the AI<->platoon registration the mod spawn skipped,
//       WITHOUT inventing new values. Option (b) (the squad leader one) is not used because the char
//       already has a valid ActivePlatoon; only the registration is missing.
//     SAFETY / THREAD: LOGIC THREAD (OnGameTick), never from the network callback. Reads and the native
//       call under SEH in a POD helper. Re-armable one-shot (ResetHostPlatoonFix). Toggle
//       kEnablePlatoonFix DEFAULT true. 250 ms throttle, bounded attempts. Runs BEFORE [AUTOTEST] so
//       the test cycle measures its effect (amIdle→0 + Task_MeleeAttack on attack).

// Toggle maestro del FIX-PLATOON. DEFAULT TRUE.
// EN: Master toggle of FIX-PLATOON. DEFAULT TRUE.
static constexpr bool kEnablePlatoonFix = true;

// Firma de Character::setActivePlatoon (0x6213F0), confirmada en bytes.
//   rcx = Character*, rdx = ActivePlatoon* (char+0x658), r8d = índice de miembro (char+0x418).
// EN: Signature of Character::setActivePlatoon (0x6213F0), confirmed in bytes.
//     rcx = Character*, rdx = ActivePlatoon* (char+0x658), r8d = member index (char+0x418).
using SetActivePlatoonFn = void(__fastcall*)(void* character, void* activePlatoon, int memberIdx);

// Estado one-shot del FIX-PLATOON (re-armable en disconnect / nueva carga).
static std::atomic<bool> s_platoonFixDone{false};
static int  s_platoonAttempts = 0;
static auto s_lastPlatoonTry  = std::chrono::steady_clock::now();
// Tope de intentos REALES (ya con host resuelto; los intentos sin host NO cuentan, ver
// FixHostPlatoonTick). Subido a 1200 (≈5 min con throttle 250ms) para no rendirse antes de
// tiempo si el ActivePlatoon/Tasker del host tarda en estar listo tras el spawn/claim.
// EN: FIX-PLATOON one-shot state (re-armable on disconnect / new load). Cap of REAL attempts (with the
//     host already resolved; attempts without a host do NOT count, see FixHostPlatoonTick). Raised to
//     1200 (≈5 min at a 250 ms throttle) so it does not give up early if the host ActivePlatoon/Tasker
//     takes a while to be ready after spawn/claim.
static constexpr int PLATOON_MAX_ATTEMPTS = 1200;

// Re-arma el FIX-PLATOON para una partida/conexión nueva (mismo patrón que FIX-COMBATCLASS).
// EN: Re-arms FIX-PLATOON for a new game/connection (same pattern as FIX-COMBATCLASS).
void ResetHostPlatoonFix() {
    s_platoonFixDone.store(false);
    s_platoonAttempts = 0;
    s_lastPlatoonTry  = std::chrono::steady_clock::now();
}

// Snapshot POD del FIX-PLATOON (vive dentro del __try; sin objetos C++ → evita C2712).
// EN: FIX-PLATOON POD snapshot (lives inside __try; no C++ objects → avoids C2712): ActivePlatoon
//     (char+0x658), AI (char+0x650), member index (char+0x418), Tasker (ActivePlatoon+0x98) before/after,
//     the AI+0x10 link before/after, platoon->me (ActivePlatoon+0x78) and flags applied/registered/
//     noPlatoon.
struct PlatoonFixSnapshot {
    uintptr_t activePlatoon  = 0;  // char+0x658 (ActivePlatoon*) — debe ser !=0 para aplicar
    uintptr_t ai             = 0;  // char+0x650 (AI*) — necesario para que el registro tenga destino
    int       memberIdx      = 0;  // char+0x418 (índice de miembro actual; se re-pasa tal cual)
    uintptr_t taskerPre      = 0;  // ActivePlatoon+0x98 (Tasker*) ANTES — evidencia (no debe cambiar)
    uintptr_t aiPlatoonPre   = 0;  // AI+0x10 (Platoon*) ANTES — el enlace omitido (típicamente 0/basura)
    uintptr_t platoonMe      = 0;  // ActivePlatoon+0x78 (Platoon* "me") — lo que el registro debe poner en AI+0x10
    uintptr_t aiPlatoonPost  = 0;  // AI+0x10 DESPUÉS — debe quedar == platoonMe (registro OK)
    uintptr_t taskerPost     = 0;  // ActivePlatoon+0x98 DESPUÉS — debe seguir == taskerPre
    int       applied        = 0;  // 1 llamado OK · -1 excepción dentro de la llamada nativa
    int       registered     = 0;  // 1 si AI+0x10 quedó == platoonMe (registro efectivo)
    int       noPlatoon      = 0;  // 1 si char+0x658==0 (aún no hay ActivePlatoon → reintentar)
};

// Lee el cluster del host, y (si procede) re-llama setActivePlatoon para forzar el registro
// AI+0x10. TODO bajo SEH. fn = setActivePlatoon (0x6213F0).
// EN: Reads the host cluster and (if appropriate) calls setActivePlatoon again to force the AI+0x10
//     registration. ALL under SEH. fn = setActivePlatoon (0x6213F0).
static void SEH_FixPlatoon(uintptr_t chr, SetActivePlatoonFn fn, PlatoonFixSnapshot* out) {
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;   // punteros del juego alineados a 8
        return true;
    };
    __try {
        if (!isHeap(chr)) return;

        // ActivePlatoon (char+0x658). Si es NULL → aún no hay platoon: reintentar (no es nuestro caso).
        // EN: ActivePlatoon (char+0x658). If NULL → no platoon yet: retry.
        uintptr_t platoon = 0;
        if (!Memory::Read(chr + 0x658, platoon) || !isHeap(platoon)) { out->noPlatoon = 1; return; }
        out->activePlatoon = platoon;

        // AI (char+0x650). Sin AI, el registro [AI+0x10] no tiene destino → no aplicar.
        // EN: AI (char+0x650). Without an AI, the [AI+0x10] registration has no target → do not apply.
        uintptr_t ai = 0;
        if (!Memory::Read(chr + 0x650, ai) || !isHeap(ai)) return;
        out->ai = ai;

        // Índice de miembro actual (char+0x418) — se re-pasa idéntico (no muta nada).
        // EN: Current member index (char+0x418) — passed back unchanged (mutates nothing).
        int32_t idx = 0;
        Memory::Read(chr + 0x418, idx);
        out->memberIdx = idx;

        // Estado ANTES: Tasker del platoon (ActivePlatoon+0x98), enlace AI+0x10 y el "me" del platoon.
        // EN: State BEFORE: platoon Tasker (ActivePlatoon+0x98), AI+0x10 link and the platoon "me".
        Memory::Read(platoon + 0x98, out->taskerPre);    // Tasker* (no debe cambiar tras el fix)
        Memory::Read(ai + 0x10, out->aiPlatoonPre);      // AI+0x10 (el enlace omitido)
        Memory::Read(platoon + 0x78, out->platoonMe);    // ActivePlatoon+0x78 = Platoon* "me"

        // Idempotencia: si el registro YA está hecho (AI+0x10 == platoon->me), no re-llamar.
        // EN: Idempotency: if the registration is ALREADY done (AI+0x10 == platoon->me), do not call again.
        if (out->aiPlatoonPre != 0 && out->aiPlatoonPre == out->platoonMe) {
            out->registered    = 1;
            out->aiPlatoonPost = out->aiPlatoonPre;
            out->taskerPost    = out->taskerPre;
            return;  // ya enlazado → applied=0, registered=1 (el orquestador cierra el one-shot)
        }

        // Re-llamar setActivePlatoon con el MISMO platoon e índice → fuerza el registro AI+0x10.
        // EN: Call setActivePlatoon again with the SAME platoon and index → forces the AI+0x10 registration.
        if (fn) {
            fn(reinterpret_cast<void*>(chr),       // rcx = Character (host)
               reinterpret_cast<void*>(platoon),   // rdx = ActivePlatoon* que YA tiene (char+0x658)
               idx);                               // r8d = índice de miembro actual (char+0x418)
            out->applied = 1;
            // Re-leer el resultado para verificar el registro.
            Memory::Read(ai + 0x10, out->aiPlatoonPost);    // debe quedar == platoonMe
            Memory::Read(platoon + 0x98, out->taskerPost);  // debe seguir == taskerPre
            out->registered = (out->aiPlatoonPost != 0 && out->aiPlatoonPost == out->platoonMe) ? 1 : 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->applied = -1;
    }
}

// Orquesta el FIX-PLATOON. Mismo patrón que FixHostCombatClassTick: throttle 250ms, one-shot
// re-armable, SOLO host, en el HILO DE LÓGICA (OnGameTick). Idempotente.
// EN: Drives FIX-PLATOON. Same pattern as FixHostCombatClassTick: 250 ms throttle, re-armable one-shot,
//     host ONLY, on the LOGIC THREAD (OnGameTick). Idempotent.
static void FixHostPlatoonTick(Core& core) {
    if (!kEnablePlatoonFix) return;                          // toggle maestro
    if (s_platoonFixDone.load()) return;                     // ya resuelto (aplicado o ya enlazado)
    if (s_platoonAttempts >= PLATOON_MAX_ATTEMPTS) return;

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastPlatoonTry < std::chrono::milliseconds(250)) return;
    s_lastPlatoonTry = now;
    // OJO: NO incrementar s_platoonAttempts aquí. El throttle de 250ms se cumple aunque
    // el host todavía no exista (Zero en menú/creando personaje). Si contásemos intentos
    // sin host, agotaríamos PLATOON_MAX_ATTEMPTS ANTES de entrar al mundo y el fix nunca
    // se aplicaría. El contador SOLO debe avanzar cuando hay un host REAL al que aplicar.
    // EN: NOTE: do NOT increment s_platoonAttempts here. The 250 ms throttle elapses even while the host
    //     does not exist yet (player in the menu / creating a character). Counting those would exhaust
    //     PLATOON_MAX_ATTEMPTS BEFORE entering the world and the fix would never apply. The counter only
    //     advances when there is a REAL host.

    uintptr_t modBase = Memory::GetModuleBase();
    if (modBase == 0) return;

    // char primario del host (mismo helper que el resto del mod / los FIX y el AUTOTEST:
    // GetPlayerPrimaryCharacterDirect = data[0] de PI+0x2B0 → host=0x... cuando está en el mundo).
    // EN: Host primary char (GetPlayerPrimaryCharacterDirect = data[0] of PI+0x2B0).
    uintptr_t hostChar = game::GetPlayerPrimaryCharacterDirect();
    if (hostChar <= 0x10000) {
        // Host aún no resoluble (menú/creación de personaje). NO contamos este intento.
        // Log de espera throttled para diagnosticar (cada ~60 llamadas ≈15s con throttle 250ms).
        static int s_platoonWaitLogs = 0;
        if ((s_platoonWaitLogs++ % 60) == 0) {
            spdlog::info("[FIX-PLATOON] esperando host en el mundo... "
                         "(GetPlayerPrimaryCharacterDirect aún <=0x10000; no se cuenta intento).");
        }
        return;                                              // char aún no resoluble → reintentar
    }

    // A partir de aquí HAY host real → este es un intento REAL: ahora sí lo contamos.
    // EN: From here on there IS a real host → this is a REAL attempt: count it now.
    s_platoonAttempts++;

    // Función nativa preferiblemente resuelta por el scanner; fallback a RVA fija (mismo patrón
    // que el [AUTOTEST] con attackTarget). RVA 0x6213F0 confirmada por AOB único en .text.
    // EN: Native function preferably resolved by the scanner; fallback to the fixed RVA (same pattern as the
    //     [AUTOTEST] with attackTarget). RVA 0x6213F0 confirmed by a unique AOB match in .text.
    SetActivePlatoonFn fn =
        reinterpret_cast<SetActivePlatoonFn>(core.GetGameFunctions().SetActivePlatoon);
    if (fn == nullptr) fn = reinterpret_cast<SetActivePlatoonFn>(modBase + 0x6213F0);

    PlatoonFixSnapshot fx{};
    SEH_FixPlatoon(hostChar, fn, &fx);

    // RVAs relativos para el log (independientes del ASLR).
    // EN: Relative RVAs for the log (ASLR-independent).
    auto rva = [modBase](uintptr_t a) -> uintptr_t { return (a > modBase) ? (a - modBase) : 0; };

    if (fx.noPlatoon) {
        // Aún no hay ActivePlatoon en char+0x658 → el spawn/claim del host no terminó: reintentar.
        // EN: No ActivePlatoon at char+0x658 yet → the host spawn/claim has not finished: retry.
        if (s_platoonAttempts <= 5 || s_platoonAttempts % 60 == 0) {
            spdlog::info("[FIX-PLATOON] aún sin ActivePlatoon (char+0x658==0) en host char=0x{:X} — "
                         "reintento {} (esperando al spawn/claim del host).", hostChar, s_platoonAttempts);
        }
        return;
    }

    if (fx.registered && fx.applied == 0) {
        // El enlace AI+0x10 YA estaba bien (idempotencia): cerrar one-shot sin tocar nada.
        // EN: The AI+0x10 link was ALREADY right (idempotency): close the one-shot without touching anything.
        s_platoonFixDone.store(true);
        spdlog::info("[FIX-PLATOON] registro AI+0x10 ya presente (AI+0x10=0x{:X} == platoon->me) en host "
                     "char=0x{:X} platoon=0x{:X} → no se re-aplica (idempotente).",
                     fx.aiPlatoonPre, hostChar, fx.activePlatoon);
        return;
    }

    if (fx.applied == 1) {
        // Log ANTES/DESPUÉS de char+0x658 (ActivePlatoon), ActivePlatoon+0x98 (Tasker) y AI+0x10.
        // EN: Log BEFORE/AFTER of char+0x658 (ActivePlatoon), ActivePlatoon+0x98 (Tasker) and AI+0x10.
        spdlog::info(
            "[FIX-PLATOON] ANTES  char=0x{:X} | ActivePlatoon(char+0x658)=0x{:X} | "
            "Tasker(AP+0x98)=0x{:X} | AI(char+0x650)=0x{:X} | AI+0x10(enlace)=0x{:X} | "
            "platoon->me(AP+0x78)=0x{:X} | memberIdx(char+0x418)={}",
            hostChar, fx.activePlatoon, fx.taskerPre, fx.ai, fx.aiPlatoonPre, fx.platoonMe, fx.memberIdx);
        spdlog::info(
            "[FIX-PLATOON] DESPUÉS char=0x{:X} | Tasker(AP+0x98)=0x{:X} (debe == antes 0x{:X}) | "
            "AI+0x10(enlace)=0x{:X} (debe == platoon->me 0x{:X}) | registro={} (1=AI<->platoon OK)",
            hostChar, fx.taskerPost, fx.taskerPre, fx.aiPlatoonPost, fx.platoonMe, fx.registered);

        if (fx.registered) {
            s_platoonFixDone.store(true);
            spdlog::info("[FIX-PLATOON] APLICADO ✓ (intento {}) — el Tasker del platoon "
                         "(0x{:X}) queda REGISTRADO con la IA del host (AI+0x10=0x{:X}). El AI tick ya "
                         "puede consumir la orden encolada → amIdle debe bajar a 0 y aparecer "
                         "Task_MeleeAttack (0x16BE448) al atacar. setActivePlatoon=0x{:X}.",
                         s_platoonAttempts, fx.taskerPost, fx.aiPlatoonPost, rva(modBase + 0x6213F0));
            core.GetNativeHud().AddSystemMessage("Pelotón del jugador re-enlazado (combate desbloqueado)");
        } else {
            // Se llamó pero AI+0x10 no quedó == platoon->me: NO cerramos el one-shot (reintentar),
            // pero sin spamear. Esto indicaría que platoon->me era 0/inesperado.
            // EN: Called but AI+0x10 did not end up == platoon->me: the one-shot is NOT closed (retry), without
            //     spamming. This would mean platoon->me was 0/unexpected.
            if (s_platoonAttempts <= 5 || s_platoonAttempts % 60 == 0) {
                spdlog::warn("[FIX-PLATOON] llamado pero registro NO confirmado (AI+0x10=0x{:X} != "
                             "platoon->me=0x{:X}) char=0x{:X} — reintento {}.",
                             fx.aiPlatoonPost, fx.platoonMe, hostChar, s_platoonAttempts);
            }
        }
        return;
    }

    if (fx.applied == -1) {
        if (s_platoonAttempts <= 5 || s_platoonAttempts % 60 == 0) {
            spdlog::warn("[FIX-PLATOON] excepción dentro de setActivePlatoon (host char=0x{:X} "
                         "platoon=0x{:X} ai=0x{:X}) — reintento {}.",
                         hostChar, fx.activePlatoon, fx.ai, s_platoonAttempts);
        }
        return;
    }

    // No aplicable aún (AI nulo, etc.) → reintentar sin spamear.
    // EN: Not applicable yet (NULL AI, etc.) → retry without spamming.
    if (s_platoonAttempts <= 5 || s_platoonAttempts % 60 == 0) {
        spdlog::info("[FIX-PLATOON] aún no aplicable (char=0x{:X} platoon=0x{:X} ai=0x{:X}) — "
                     "reintento {}.", hostChar, fx.activePlatoon, fx.ai, s_platoonAttempts);
    }
}

// ══════════════════════════════════════════════════════════════════════════════════
// [AUTOTEST] — Auto-test de combate del host (dispara attackTarget POR CÓDIGO)
// ══════════════════════════════════════════════════════════════════════════════════
// OBJETIVO: que Zero entre UNA vez (crea personaje, se acerca a unos bandidos) y el mod
// dispare solo un ataque del host contra un NPC enemigo cercano — SIN dar órdenes por GUI —
// y loguee TODO el estado antes/después para confirmar si el FIX-HOSTILITY (defaultRelation
// FR+0x60=-100) desbloqueó realmente el combate.
//
// QUÉ HACE (UNA sola vez por carga, idempotente, throttled, SOLO host, hilo de lógica):
//   1) Resuelve el char primario del host (GetPlayerPrimaryCharacterDirect = PI+0x2B0 data[0]).
//   2) Recorre la LISTA DE SIMULACIÓN (GW+0x768/+0x788/+0x770) buscando un NPC de facción
//      DISTINTA a la del host y CERCANO (pos char+0x48, dist <= kAutotestRadius). Igual que
//      hacen los DIAG de combate, pero filtrando por facción y distancia.
//   3) Snapshot PRE: isEnemy nativo(0x6B26D0) host->npc, amIdle(CharBody+0x70),
//      activeTask(*(CharBody+0x68)) + su vtbl, currentTarget(CombatClass+0x290).
//   4) Llama a Character::attackTarget(0x5CB0A0)(host, npc) bajo SEH — encola el ataque.
//   5) Marca el test "disparado" y arranca un contador de ticks; durante kAutotestPostWindow
//      vuelve a leer el estado POST (amIdle, activeTask ¿= Task_MeleeAttack?, currentTarget)
//      y emite el VEREDICTO: ¿se encoló el ataque? ¿el char ataca de verdad?
//
// FIRMA de attackTarget (RE de bytes Steam 1.0.68, RVA 0x5CB0A0, audit-12/14 ✅):
//   void __fastcall Character::attackTarget(Character* me /*rcx*/, Character* who /*rdx*/);
//   Prólogo real: `48 85 D2` (test rdx,rdx) → la propia función ya valida who!=NULL y sale si
//   es NULL; luego `48 89 5C 24 08 57 48 83 EC 40` (prólogo estándar, NO mov-rax-rsp). Por eso
//   la LLAMAMOS directamente (no es un hook/detour) protegida con SEH — no requiere el fix
//   MovRaxRsp. Deref interno: char+0x650 (AI*) +0x78.
//
// SEGURIDAD / THREAD: se ejecuta en el HILO DE LÓGICA (OnGameTick), igual que FIX-CONTROL/
// FIX-HOSTILITY (NUNCA desde el callback de red). Toda lectura y la llamada nativa van bajo
// SEH en helpers POD (sin objetos C++ → evita C2712). Si el char se liberó o la memoria no es
// accesible, falla con gracia (no crashea, reintenta o se rinde). One-shot re-armable en
// disconnect / nueva carga (ResetHostCombatAutotest).
//
// RIESGO asumido (honesto): el auto-test INVOCA una función del motor (attackTarget) sobre el
// char del host en el hilo de lógica. attackTarget muta el AITaskSytem del char (encola un Job).
// Es exactamente lo que hace el motor cuando clicas un enemigo, así que es seguro EN PRINCIPIO,
// pero llamarla nosotros añade una ruta nueva. Mitigaciones: (a) SEH alrededor de la llamada;
// (b) validación estricta de punteros (host/npc heap-ptr alineados, vtable CharBody conocida);
// (c) una sola invocación por carga; (d) toggle para desactivar; (e) requiere isEnemy nativo
// previo legible (no atacamos si no podemos siquiera evaluar hostilidad).
// EN: [AUTOTEST] — Host combat auto-test (fires attackTarget FROM CODE).
//     GOAL: the player enters ONCE (creates a character, walks near some NPCs) and the mod fires a host
//       attack against a nearby enemy NPC by itself — WITHOUT GUI orders — logging ALL state
//       before/after to confirm whether FIX-HOSTILITY (defaultRelation FR+0x60=-100) really unblocked
//       combat.
//     WHAT IT DOES (ONCE per load, idempotent, throttled, host ONLY, logic thread): 1) resolves the host
//       primary char; 2) walks the SIMULATION LIST (GW+0x768/+0x788/+0x770) looking for NPCs of a
//       DIFFERENT faction within kAutotestRadius (position at char+0x48); 3) PRE snapshot: native isEnemy
//       (0x6B26D0), amIdle (CharBody+0x70), activeTask (*(CharBody+0x68)) + its vtable, currentTarget
//       (CombatClass+0x290); 4) calls Character::attackTarget (0x5CB0A0)(host, npc) under SEH (queues the
//       attack); 5) during kAutotestPostWindow it re-reads the POST state and emits the VERDICT.
//     attackTarget SIGNATURE (byte RE Steam 1.0.68, RVA 0x5CB0A0): void __fastcall(Character* me,
//       Character* who). Real prologue `48 85 D2` (test rdx,rdx: validates who itself), then a standard
//       prologue (NOT mov-rax-rsp). So it is CALLED directly (not hooked) under SEH. Internal deref:
//       char+0x650 (AI*) +0x78.
//     SAFETY / THREAD: LOGIC THREAD (OnGameTick), never from the network callback; all reads and the
//       native call under SEH in POD helpers; fails gracefully. Re-armable one-shot
//       (ResetHostCombatAutotest).
//     ACCEPTED RISK: it INVOKES an engine function on the host char (mutates its AITaskSystem, queues a
//       Job) — the same the engine does when you click an enemy. Mitigations: SEH, strict pointer
//       validation, one call per load, toggle, requires a readable native isEnemy first.

// ── Forward declarations: el AUTO-TEST reutiliza helpers SEH definidos MÁS ABAJO en este
//    mismo archivo (sección Fase 4). Se declaran aquí para poder usarlos desde el bloque
//    del auto-test sin reordenar el archivo. Las definiciones reales están intactas abajo. ──
// EN: ── Forward declarations: the AUTO-TEST reuses SEH helpers defined FURTHER DOWN in this file
//     (Phase 4 section). Declared here so the auto-test block can use them without reordering. ──
using IsEnemyFn = bool(__fastcall*)(uintptr_t factionRelations, uintptr_t otherFaction); // def. abajo
static int  SEH_CallIsEnemy(IsEnemyFn fn, uintptr_t fr, uintptr_t otherFaction);          // def. abajo
static void SEH_ReadCharName(uintptr_t chrPtr, char* dst, size_t dstSize);                // def. abajo

// Toggle maestro del AUTO-TEST de combate. DEFAULT TRUE (lo pidió Zero para mañana).
// Poner a false para desactivar por completo el disparo automático del ataque.
// EN: Master toggle of the combat AUTO-TEST. DEFAULT TRUE. Set to false to fully disable the automatic
//     attack.
static constexpr bool kEnableCombatAutotest = true;

// Radio (en unidades de mundo Kenshi) para considerar un NPC "cercano". Generoso para que
// baste con acercarse a CUALQUIER NPC (ya no solo bandidos); evita elegir uno al otro lado del
// mapa. Subido 60→80 (2026-06-19) al quitar el filtro de bandido: maximiza que dispare.
// EN: Radius (Kenshi world units) for a "nearby" NPC. Generous so approaching ANY NPC is enough; avoids
//     picking one across the map. Raised 60→80 (2026-06-19) when the bandit filter was dropped.
static constexpr float kAutotestRadius = 80.0f;

// Ventana POST de muestreo. attackTarget SOLO encola; el AI tick materializa currentTarget/
// Task_MeleeAttack en un tick POSTERIOR. Por eso NO leemos una sola vez: muestreamos durante
// kAutotestPostWindow ticks y damos veredicto EN CUANTO el host adquiere objetivo (currentTarget!=0
// o Task_MeleeAttack), o al agotar la ventana. kAutotestPostMinTicks = espera mínima antes del 1er
// muestreo (deja correr ≥1 tick de lógica para que el AI consuma la cola).
// EN: POST sampling window. attackTarget ONLY queues; the AI tick materializes currentTarget /
//     Task_MeleeAttack on a LATER tick. So it samples for kAutotestPostWindow ticks and gives the verdict
//     AS SOON AS the host acquires a target, or when the window expires. kAutotestPostMinTicks = minimum
//     wait before the first sample (let ≥1 logic tick run).
static constexpr int kAutotestPostMinTicks = 3;    // espera mínima antes del primer muestreo
static constexpr int kAutotestPostWindow   = 90;   // ~ varios segundos de ventana de adquisición

// Número máximo de intentos de BÚSQUEDA del NPC enemigo antes de rendirse (throttle 250ms →
// ~2 minutos buscando). Se rinde sin ruido si no aparece ningún enemigo cercano.
// EN: Max SEARCH attempts for an enemy NPC before giving up (250 ms throttle → ~2 minutes). Gives up
//     quietly if no nearby enemy appears.
static constexpr int AUTOTEST_MAX_ATTEMPTS = 480;

// attackTarget nativo (RVA 0x5CB0A0): void __fastcall(Character* me, Character* who).
// EN: Native attackTarget (RVA 0x5CB0A0): void __fastcall(Character* me, Character* who).
using AttackTargetFn = void(__fastcall*)(void* me, void* who);

// Estado del AUTO-TEST (re-armable en disconnect / nueva carga vía ResetHostCombatAutotest).
//   s_autotestFired: ya se invocó attackTarget (no repetir).
//   s_autotestDone:  ciclo completo (PRE + attack + POST logueado) → no hacer nada más.
// EN: AUTO-TEST state (re-armable on disconnect / new load via ResetHostCombatAutotest).
//       s_autotestFired: attackTarget already invoked (do not repeat).
//       s_autotestDone:  full cycle (PRE + attack + POST logged) → nothing more to do.
static std::atomic<bool> s_autotestFired{false};
static std::atomic<bool> s_autotestDone{false};
static int   s_autotestAttempts   = 0;
static int   s_autotestPostCounter = 0;          // ticks transcurridos desde el disparo
static uintptr_t s_autotestHostChar = 0;          // char del host usado en el disparo (para POST)
static uintptr_t s_autotestNpcChar  = 0;          // 1er NPC de la tanda (referencia, para POST)
static int   s_autotestTargetCount = 0;          // nº de NPCs atacados en la tanda (veredicto global)
static auto  s_lastAutotestTry     = std::chrono::steady_clock::now();

// Re-arma el AUTO-TEST para una partida/conexión nueva (mismo patrón que los FIX).
// EN: Re-arms the AUTO-TEST for a new game/connection (same pattern as the FIXes).
void ResetHostCombatAutotest() {
    s_autotestFired.store(false);
    s_autotestDone.store(false);
    s_autotestAttempts    = 0;
    s_autotestPostCounter = 0;
    s_autotestHostChar    = 0;
    s_autotestNpcChar     = 0;
    s_autotestTargetCount = 0;
    s_lastAutotestTry     = std::chrono::steady_clock::now();
}

// Snapshot POD del estado de combate de UN char (host o npc), leído bajo SEH.
// Reutiliza los MISMOS offsets que el [DIAG-ATTACK]/[DIAG-COMBATSTRUCT] (ya verificados):
//   CharBody @char+0x648 · CombatClass = *(CharBody+0x8) · currentTarget @CombatClass+0x290 ·
//   activeTask = *(CharBody+0x68) (CharBody.currentAction) · amIdle @CharBody+0x70 (bool).
// EN: POD snapshot of the combat state of ONE char (host or npc), read under SEH. Same offsets as
//     [DIAG-ATTACK]/[DIAG-COMBATSTRUCT] (already verified): CharBody @char+0x648 · CombatClass =
//     *(CharBody+0x8) · currentTarget @CombatClass+0x290 · activeTask = *(CharBody+0x68)
//     (CharBody.currentAction) · amIdle @CharBody+0x70 (bool).
struct AutotestCharState {
    uintptr_t body          = 0;   // char+0x648 (CharBody*)
    uintptr_t combat        = 0;   // *(CharBody+0x8) (CombatClass*) — 0 = sin CombatClass (no ataca)
    uintptr_t currentTarget = 0;   // CombatClass+0x290 — != 0 cuando hay objetivo de ataque activo
    uintptr_t activeTask    = 0;   // *(CharBody+0x68) (Tasker activo) — 0 = sin tarea
    uintptr_t activeTaskVtbl= 0;   // vtable del Tasker activo (comparar vs Task_MeleeAttack 0x16BE448)
    int       amIdle        = -1;  // CharBody+0x70 (bool): 1=ocioso, 0=ejecutando Task, -1=no leído
    int       read          = 0;   // 1 si se leyó el bloque OK
};

// Lee el estado de combate de un char (SEH-aislado, POD). isHeap valida punteros del juego.
// EN: Reads the combat state of a char (SEH-isolated, POD). isHeap validates game pointers.
static void SEH_ReadAutotestState(uintptr_t chr, AutotestCharState* out,
                                  bool (*isHeap)(uintptr_t)) {
    if (chr <= 0x10000) return;
    __try {
        uintptr_t vtbl = 0;
        uintptr_t body = 0;
        if (!Memory::Read(chr + 0x648, body) || !isHeap(body)) return;
        out->body = body;
        // CombatClass = *(CharBody+0x8). Si es 0 → el char no tiene capa de combate (no atacará).
        // EN: CombatClass = *(CharBody+0x8). If 0 → the char has no combat layer (will not attack).
        uintptr_t combat = 0;
        if (Memory::Read(body + 0x8, combat) && isHeap(combat)) {
            out->combat = combat;
            // currentTarget = CombatClass+0x290 (lo escribe setAttackTarget 0x665580).
            // EN: currentTarget = CombatClass+0x290 (written by setAttackTarget 0x665580).
            Memory::Read(combat + 0x290, out->currentTarget);
        }
        // Task activo = *(CharBody+0x68) (CharBody.currentAction). vtbl → ¿Task_MeleeAttack?
        // EN: Active task = *(CharBody+0x68) (CharBody.currentAction). vtbl → Task_MeleeAttack?
        uintptr_t activeTask = 0;
        if (Memory::Read(body + 0x68, activeTask) && isHeap(activeTask)) {
            out->activeTask = activeTask;
            if (Memory::Read(activeTask, vtbl)) out->activeTaskVtbl = vtbl;
        }
        // amIdle = CharBody+0x70 (bool): 1 ocioso (gate rechazó), 0 ejecutando un Task.
        // EN: amIdle = CharBody+0x70 (bool): 1 idle (gate rejected), 0 running a Task.
        uint8_t idle = 0;
        if (Memory::Read(body + 0x70, idle)) out->amIdle = idle ? 1 : 0;
        out->read = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Cap de NPCs a atacar en la TANDA del auto-test (2026-06-19, ampliación multi-objetivo).
// Zero se mete en una zona con gente y atacamos a TODOS los NPCs de facción distinta cercanos,
// no solo al más cercano. Cap a 8 por rendimiento: evita disparar attackTarget decenas de veces
// (cada llamada encola una orden en el Tasker; 8 es suficiente para tener confianza estadística).
// EN: Cap of NPCs to attack in the auto-test BATCH (2026-06-19, multi-target extension). The player
//     walks into an area with people and ALL nearby NPCs of a different faction are attacked, not only
//     the closest. Capped at 8 for performance (each call queues an order in the Tasker).
static constexpr int kAutotestMaxTargets = 8;

// Datos POD de UN objetivo de la tanda (para loguear cada NPC atacado).
// EN: POD data of ONE batch target (to log each attacked NPC): pointer, name, faction (npc+0x10),
//     fundamentalType (faction+0x34: 8=bandit, 7=slaver...), distance, native isEnemy result and
//     attackTarget status.
struct AutotestTarget {
    uintptr_t npcChar    = 0;     // Character* del NPC objetivo
    char      npcName[32]= {0};   // nombre del NPC (log)
    uintptr_t npcFaction = 0;     // npc+0x10
    int       npcFundType= -99;   // npc faction+0x34 (fundamentalType): 8=bandido,7=esclavista,...
    float     npcDist    = -1.f;  // distancia host<->npc (unidades de mundo)
    int       hostEnemyNpc = -1;  // isEnemy nativo(hostFR, npcFaction): -1 no llamado, 0 no, 1 sí
    int       attackCalled = 0;   // 1 si attackTarget(host,npc) se invocó OK, -1 si excepción
};

// Snapshot POD de la FASE DE BÚSQUEDA + DISPARO del auto-test (todo bajo SEH).
// AMPLIADO (2026-06-19): de UN objetivo a una TANDA de hasta kAutotestMaxTargets NPCs.
// EN: POD snapshot of the auto-test SEARCH + FIRE phase (all under SEH). EXTENDED (2026-06-19): from ONE
//     target to a BATCH of up to kAutotestMaxTargets NPCs.
struct AutotestSnapshot {
    int       resolved   = 0;     // 1 si se resolvió host + facción del host
    uintptr_t hostChar   = 0;     // char primario del host
    uintptr_t hostFaction= 0;     // host+0x10
    uintptr_t hostFR     = 0;     // hostFaction+0x78 (FactionRelations*)

    // TANDA de NPCs de facción distinta cercanos (ordenada por cercanía, cap kAutotestMaxTargets).
    // EN: BATCH of nearby NPCs of a different faction (sorted by distance, capped at kAutotestMaxTargets).
    AutotestTarget targets[kAutotestMaxTargets]{};
    int       targetCount = 0;    // nº de NPCs realmente recogidos en la tanda (0..kAutotestMaxTargets)
    int       attackedCount = 0;  // nº de attackTarget invocados OK en la tanda (para el log)

    int       scanned    = 0;     // nodos de la simlist recorridos (sanity)
    int       banditsSeen= 0;     // nº de candidatos de facción distinta vistos en el barrido
    float     hostDefaultRel = 0.f; // hostFR+0x60 (defaultRelation; tras FIX-HOSTILITY debe ser -100)

    // Tasker del host (char+0x658 ActivePlatoon → +0x98). Es el destino REAL del encolado de la
    // orden de ataque (0x6744A0 → getTasker 0x791B10 → [+0x98] → 0x674300 pushOrder). Si es 0,
    // la orden no tiene a dónde encolarse (ruta rota); si !=0, el encolado tiene destino válido.
    // EN: Host Tasker (char+0x658 ActivePlatoon → +0x98). The REAL destination of the attack order enqueue
    //     (0x6744A0 → getTasker 0x791B10 → [+0x98] → 0x674300 pushOrder). If 0 the order has nowhere to go
    //     (broken path); if !=0 the enqueue has a valid destination.
    uintptr_t hostTasker = 0;     // char+0x658 → +0x98 (Tasker*) — evidencia de ruta de encolado

    // PRE (estado de combate del host justo antes de atacar la tanda).
    // EN: PRE (host combat state right before attacking the batch).
    AutotestCharState pre{};
};

// Busca un NPC enemigo cercano en la simlist y, si lo encuentra y es hostil, DISPARA
// attackTarget(host, npc). TODO bajo SEH (POD, sin objetos C++ → C2712). No escribe NADA
// en estructuras del motor salvo la llamada nativa a attackTarget (que el motor también hace).
// EN: Looks for nearby enemy NPCs in the simlist and, if found and hostile, FIRES attackTarget(host, npc).
//     ALL under SEH (POD, no C++ objects → C2712). It writes NOTHING into engine structures except the
//     native attackTarget call (which the engine itself also makes).
static void SEH_RunAutotest(uintptr_t modBase, uintptr_t gwObj, uintptr_t hostChar,
                            uintptr_t hostFaction, AttackTargetFn attackFn, IsEnemyFn isEnemyFn,
                            float radius, AutotestSnapshot* out) {
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;   // punteros del juego alineados a 8
        return true;
    };
    __try {
        if (!isHeap(hostChar) || !isHeap(hostFaction)) return;
        out->hostChar = hostChar;
        out->hostFaction = hostFaction;
        out->resolved = 1;

        // FactionRelations del host (Faction+0x78) + defaultRelation (FR+0x60) para el log.
        // EN: Host FactionRelations (Faction+0x78) + defaultRelation (FR+0x60) for the log.
        uintptr_t hostFR = 0;
        if (Memory::Read(hostFaction + 0x78, hostFR) && isHeap(hostFR)) {
            out->hostFR = hostFR;
            Memory::Read(hostFR + 0x60, out->hostDefaultRel);
        }

        // Posición del host (char+0x48 = Vec3 cacheada) para medir cercanía.
        // EN: Host position (char+0x48 = cached Vec3) to measure distance.
        float hx = 0.f, hy = 0.f, hz = 0.f;
        Memory::Read(hostChar + 0x48 + 0, hx);
        Memory::Read(hostChar + 0x48 + 4, hy);
        Memory::Read(hostChar + 0x48 + 8, hz);

        // Recorrer la LISTA DE SIMULACIÓN (igual que SEH_ReadCombatStructDiag). count=[GW+0x770],
        // array=[GW+0x788], idx=[GW+0x768], nodo: +0x00 next, +0x10 Character*.
        //
        // ⚠ CAMBIO AUTOTEST (2026-06-19, MULTI-OBJETIVO): antes elegíamos UN solo NPC (el más
        //   cercano). Ahora recogemos una TANDA de hasta kAutotestMaxTargets NPCs de facción
        //   distinta dentro del radio, ORDENADOS por cercanía (los N más cercanos), para atacar a
        //   VARIOS y ganar confianza sobre si el FIX-PLATOON desbloquea el combate. Sin filtro de
        //   fundamentalType: vale cualquier facción != host (se guarda el tipo solo para el log).
        //
        // Selección de los N más cercanos: array local ordenado por distancia ascendente con
        // inserción acotada (cap kAutotestMaxTargets). Sin asignaciones dinámicas (POD, dentro del
        // __try). candDist/candNpc/candFac/candFT corren en paralelo (mismo índice = mismo NPC).
        // EN: Walk the SIMULATION LIST (same as SEH_ReadCombatStructDiag). count=[GW+0x770], array=[GW+0x788],
        //     idx=[GW+0x768], node: +0x00 next, +0x10 Character*.
        //     AUTOTEST CHANGE (2026-06-19, MULTI-TARGET): before, ONE NPC (the closest) was chosen. Now a BATCH
        //     of up to kAutotestMaxTargets NPCs of a different faction inside the radius is collected, SORTED by
        //     distance (the N closest). No fundamentalType filter: any faction != host (the type is kept for
        //     the log only).
        //     Picking the N closest: local array sorted by ascending distance with bounded insertion (capped at
        //     kAutotestMaxTargets). No dynamic allocations (POD, inside __try). candDist/candNpc/candFac/candFT
        //     run in parallel (same index = same NPC).
        float     candDist[kAutotestMaxTargets];
        uintptr_t candNpc[kAutotestMaxTargets];
        uintptr_t candFac[kAutotestMaxTargets];
        int       candFT[kAutotestMaxTargets];
        for (int i = 0; i < kAutotestMaxTargets; ++i) {
            candDist[i] = radius + 1.f; candNpc[i] = 0; candFac[i] = 0; candFT[i] = -99;
        }
        int candN = 0;                 // nº de candidatos actualmente en el top-N
        int candidatesSeen = 0;        // nº de candidatos de facción distinta vistos (sanity)
        if (gwObj != 0) {
            uint64_t count = 0;
            if (Memory::Read(gwObj + 0x770, count) && count > 0) {
                uintptr_t arrayPtr = 0, headIdx = 0;
                if (Memory::Read(gwObj + 0x788, arrayPtr) && arrayPtr > 0x10000 &&
                    Memory::Read(gwObj + 0x768, headIdx)) {
                    uintptr_t node = 0;
                    if (Memory::Read(arrayPtr + headIdx * 8, node)) {
                        const uint32_t kMaxIter = 20000;
                        uint32_t walked = 0;
                        while (node > 0x10000 && walked < kMaxIter) {
                            walked++;
                            uintptr_t chr = 0;
                            if (Memory::Read(node + 0x10, chr) && isHeap(chr) && chr != hostChar) {
                                // Facción del candidato (char+0x10). Solo NPCs de facción DISTINTA.
                                // EN: Candidate faction (char+0x10). Only NPCs of a DIFFERENT faction.
                                uintptr_t fac = 0;
                                if (Memory::Read(chr + 0x10, fac) && isHeap(fac) && fac != hostFaction) {
                                    // Distancia al host (pos char+0x48).
                                    // EN: Distance to the host (position char+0x48).
                                    float nx = 0, ny = 0, nz = 0;
                                    Memory::Read(chr + 0x48 + 0, nx);
                                    Memory::Read(chr + 0x48 + 4, ny);
                                    Memory::Read(chr + 0x48 + 8, nz);
                                    float dx = nx - hx, dy = ny - hy, dz = nz - hz;
                                    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                                    // Solo dentro del radio entran en la tanda.
                                    // EN: Only those inside the radius join the batch.
                                    if (dist <= radius) {
                                        candidatesSeen++;
                                        // ¿Cabe / mejora al peor del top-N? El peor está al final
                                        // (array ordenado ascendente). Si hay hueco o este es más
                                        // cercano que el último, insertamos manteniendo el orden.
                                        // EN: Does it fit / beat the worst of the top-N? The worst is at the end (ascending array). If there is
                                        //     room or this one is closer than the last, insert keeping the order.
                                        if (candN < kAutotestMaxTargets ||
                                            dist < candDist[kAutotestMaxTargets - 1]) {
                                            int32_t ft = -99;
                                            Memory::Read(fac + 0x34, ft);  // tipo solo para el log
                                            // Posición de inserción (primer slot con dist mayor).
                                            // EN: Insertion position (first slot with a larger distance); shift the worse ones right.
                                            int pos = (candN < kAutotestMaxTargets)
                                                          ? candN
                                                          : (kAutotestMaxTargets - 1);
                                            // Desplazar a la derecha los peores para hacer hueco.
                                            while (pos > 0 && candDist[pos - 1] > dist) {
                                                candDist[pos] = candDist[pos - 1];
                                                candNpc[pos]  = candNpc[pos - 1];
                                                candFac[pos]  = candFac[pos - 1];
                                                candFT[pos]   = candFT[pos - 1];
                                                pos--;
                                            }
                                            candDist[pos] = dist; candNpc[pos] = chr;
                                            candFac[pos]  = fac;  candFT[pos]  = ft;
                                            if (candN < kAutotestMaxTargets) candN++;
                                        }
                                    }
                                }
                            }
                            uintptr_t next = 0;
                            if (!Memory::Read(node + 0x00, next)) break;
                            if (next == node) break;
                            node = next;
                        }
                        out->scanned = static_cast<int>(walked);
                    }
                }
            }
        }
        out->banditsSeen = candidatesSeen;   // nº de NPCs de facción distinta vistos en el radio

        // Si NO hay NINGÚN NPC de facción distinta dentro del radio, NO disparamos (esperamos a
        // que Zero se acerque a una zona con gente).
        // EN: If there is NO NPC of a different faction inside the radius, do NOT fire (wait for the player to
        //     approach an area with people).
        if (candN == 0) {
            return;   // no hay NPCs de otra facción cerca todavía → reintentar luego
        }

        // TASKER del host: char+0x658 (ActivePlatoon*) → +0x98 (Tasker*). Es el destino REAL del
        // encolado de la orden de ataque (RE 2026-06-19: 0x6744A0 → getTasker 0x791B10 [char+0x658]
        // → [+0x98] → pushOrder 0x674300). Lo leemos como EVIDENCIA de que la ruta de encolado
        // tiene a dónde llegar. Si es 0, attackTarget no podría encolar nada (ruta sin destino).
        // EN: Host TASKER: char+0x658 (ActivePlatoon*) → +0x98 (Tasker*). The REAL destination of the attack
        //     order enqueue (RE 2026-06-19). Read as EVIDENCE that the enqueue path has somewhere to land; if 0,
        //     attackTarget could not queue anything.
        {
            uintptr_t ap = 0;
            if (Memory::Read(hostChar + 0x658, ap) && isHeap(ap)) {
                uintptr_t tasker = 0;
                if (Memory::Read(ap + 0x98, tasker) && isHeap(tasker)) {
                    out->hostTasker = tasker;
                    // [DIAG-PUSHORDER] Publica el Tasker del host para que el hook de pushOrder
                    // marque isHost=1 cuando la orden de ataque (mode=4) entre en ESTE Tasker.
                    // EN: [DIAG-PUSHORDER] Publish the host Tasker so the pushOrder hook marks isHost=1 when the attack
                    //     order (mode=4) enters THIS Tasker.
                    combat_hooks::g_hostTaskerForDiag.store(tasker, std::memory_order_release);
                }
            }
        }

        // PRE: estado de combate del host ANTES de atacar la tanda (una sola lectura, vale para todos).
        // EN: PRE: host combat state BEFORE attacking the batch (a single read, valid for all).
        SEH_ReadAutotestState(hostChar, &out->pre, isHeap);

        // DISPARO de la TANDA: por cada NPC del top-N, llenamos su AutotestTarget (nombre, tipo,
        // isEnemy nativo) y llamamos attackTarget(host, npc). attackTarget valida internamente
        // who!=NULL (test rdx) y SOLO ENCOLA una orden (mode 4); el AI tick la materializa después.
        // Atacar a varios NO cambia el POST (medimos el ESTADO DEL HOST tras la tanda): basta con
        // que el host salga de ocioso por cualquiera de las órdenes encoladas.
        // EN: FIRE the BATCH: for each top-N NPC fill its AutotestTarget (name, type, native isEnemy) and call
        //     attackTarget(host, npc). attackTarget validates who!=NULL itself and ONLY QUEUES an order (mode 4);
        //     the AI tick materializes it later. Attacking several does not change the POST (the HOST state is
        //     measured after the batch): it is enough that the host leaves idle for any of the queued orders.
        out->targetCount = candN;
        for (int i = 0; i < candN; ++i) {
            AutotestTarget* t = &out->targets[i];
            t->npcChar    = candNpc[i];
            t->npcFaction = candFac[i];
            t->npcFundType= candFT[i];
            t->npcDist    = candDist[i];
            SEH_ReadCharName(candNpc[i], t->npcName, sizeof(t->npcName));
            // Hostilidad nativa host->npc (informativo; atacamos igual pase lo que pase).
            // EN: Native host->npc hostility (informative; we attack regardless).
            if (isEnemyFn && out->hostFR != 0) {
                t->hostEnemyNpc = SEH_CallIsEnemy(isEnemyFn, out->hostFR, candFac[i]);
            }
            // Llamada nativa a attackTarget para ESTE objetivo (cada una protegida por el __try
            // global; si una peta, marcamos -1 en ese objetivo y seguimos con los demás no es
            // posible bajo un solo __try, así que ante excepción salimos por el __except global).
            // EN: Native attackTarget call for THIS target. A single __try covers the whole batch, so an exception
            //     exits through the global __except instead of marking just that target.
            if (attackFn) {
                attackFn(reinterpret_cast<void*>(hostChar), reinterpret_cast<void*>(candNpc[i]));
                t->attackCalled = 1;
                out->attackedCount++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Excepción en la búsqueda o en alguna llamada nativa de la tanda. attackedCount conserva
        // cuántos se dispararon OK antes del fallo; los no marcados quedan en attackCalled=0.
        // EN: Exception during the search or a native call of the batch. attackedCount keeps how many fired OK
        //     before the failure; unmarked ones stay at attackCalled=0.
        out->targets[0].attackCalled = -1;   // señal de excepción para el orquestador
    }
}

// Orquesta el AUTO-TEST. Mismo patrón que los FIX: throttle 250ms, one-shot re-armable,
// SOLO host, HILO DE LÓGICA (OnGameTick). Dos fases: (1) buscar+disparar, (2) leer POST.
// EN: Drives the AUTO-TEST. Same pattern as the FIXes: 250 ms throttle, re-armable one-shot, host ONLY,
//     LOGIC THREAD (OnGameTick). Two phases: (1) search + fire, (2) read POST.
static void CombatAutotestTick(Core& core) {
    if (!kEnableCombatAutotest) return;          // toggle maestro
    if (s_autotestDone.load()) return;           // ciclo completo ya logueado

    uintptr_t modBase = Memory::GetModuleBase();
    if (modBase == 0) return;

    // Validador de heap-ptr (mismo criterio que el resto del mod) — para leer POST fuera del SEH.
    // EN: Heap pointer validator (same criterion as the rest of the mod) — to read POST outside the SEH.
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        return true;
    };
    // ES: RVAs de vtables de tareas del motor: 0x16BE448 = Task_MeleeAttack (ataque automático) y
    //     0x16BF9E8 = Task_FocusedMeleeAttack (ataque ordenado por el jugador).
    // EN: Engine task vtable RVAs: 0x16BE448 = Task_MeleeAttack (automatic attack) and
    //     0x16BF9E8 = Task_FocusedMeleeAttack (player-ordered attack).
    const uintptr_t taskMeleeVtblAbs        = modBase + 0x16BE448; // Task_MeleeAttack (ataque AUTOMÁTICO, audit-12)
    const uintptr_t taskFocusedMeleeVtblAbs = modBase + 0x16BF9E8; // Task_FocusedMeleeAttack (ataque ORDENADO, RE 2026-07-14)

    // ── FASE 2: POST — MUESTREO EN VENTANA (RE 2026-06-19) ──
    // attackTarget SOLO encola la orden; el AI tick materializa currentTarget(+0x290)/Task_MeleeAttack
    // en un tick POSTERIOR. Por eso muestreamos cada tick durante una ventana y damos veredicto
    // POSITIVO en cuanto el host adquiere objetivo, o NEGATIVO al agotar la ventana. Así NO damos un
    // falso "ocioso" por leer demasiado pronto (que era el error del muestreo de 1 sola lectura).
    // EN: ── PHASE 2: POST — WINDOWED SAMPLING (RE 2026-06-19) ── attackTarget ONLY queues the order; the AI
    //     tick materializes currentTarget(+0x290)/Task_MeleeAttack on a LATER tick. So it samples every tick
    //     during a window and gives a POSITIVE verdict as soon as the host acquires a target, or NEGATIVE when
    //     the window runs out (avoids a false "idle" from reading too early).
    if (s_autotestFired.load()) {
        s_autotestPostCounter++;
        if (s_autotestPostCounter < kAutotestPostMinTicks) return;  // espera mínima (deja correr el AI)

        AutotestCharState post{};
        SEH_ReadAutotestState(s_autotestHostChar, &post, isHeap);
        // ÉXITO = cualquiera de las dos tareas de ataque: automática (Task_MeleeAttack) u ordenada
        // (Task_FocusedMeleeAttack, clic manual). Antes solo se reconocía la automática → falso negativo.
        // EN: SUCCESS = either attack task: automatic (Task_MeleeAttack) or ordered (Task_FocusedMeleeAttack,
        //     manual click). Before, only the automatic one was recognized → false negative.
        bool meleeAutoNow    = (post.activeTaskVtbl == taskMeleeVtblAbs);
        bool meleeFocusedNow = (post.activeTaskVtbl == taskFocusedMeleeVtblAbs);
        bool meleeNow  = (meleeAutoNow || meleeFocusedNow); // el host ejecuta ALGÚN ataque melee
        bool hasTarget = (post.currentTarget != 0);
        // RVA relativo del activeTaskVtbl (para identificar la tarea sin depender del ASLR del log).
        // EN: Relative RVA of activeTaskVtbl (identifies the task independently of ASLR).
        uintptr_t activeTaskRva = (post.activeTaskVtbl > modBase) ? (post.activeTaskVtbl - modBase) : 0;

        // ¿El host salió de ocioso? → VEREDICTO GLOBAL POSITIVO (no esperamos a agotar la ventana).
        // amIdle=0 + (Task_MeleeAttack o currentTarget!=0) = el host EJECUTA un ataque contra alguno
        // de los NPCs de la tanda → el FIX-PLATOON desbloqueó el combate.
        // EN: Did the host leave idle? → GLOBAL POSITIVE VERDICT (no need to wait for the window).
        //     amIdle=0 + (Task_MeleeAttack or currentTarget!=0) = the host IS attacking one of the batch NPCs.
        bool outOfIdle = (post.amIdle == 0);
        if ((meleeNow || hasTarget) && outOfIdle) {
            // Distinguimos el tipo de ataque para diagnósticos futuros: auto=Task_MeleeAttack (0x16BE448),
            // ordenado=Task_FocusedMeleeAttack (0x16BF9E8), o solo currentTarget!=0 (aún sin tarea melee).
            // EN: Distinguish the attack type for future diagnostics: auto / ordered / currentTarget only.
            const char* meleeKind = meleeAutoNow ? "automatico(Task_MeleeAttack)"
                                  : meleeFocusedNow ? "ordenado(Task_FocusedMeleeAttack)"
                                  : "sin-tarea-melee(solo currentTarget)";
            spdlog::info(
                "[AUTOTEST] POST OK (tick {} de la ventana, {} objetivos atacados) host=0x{:X} | "
                "combat=0x{:X} currentTarget=0x{:X} | activeTask=0x{:X} vtblRVA=0x{:X} "
                "(melee={} tipo={} | Task_MeleeAttack=0x16BE448 / Task_FocusedMeleeAttack=0x16BF9E8) | amIdle={}",
                s_autotestPostCounter, s_autotestTargetCount, s_autotestHostChar, post.combat,
                post.currentTarget, post.activeTask, activeTaskRva, meleeNow ? 1 : 0, meleeKind, post.amIdle);
            spdlog::info("[AUTOTEST] ==> *** COMBATE DESBLOQUEADO *** el host SALIÓ DE OCIOSO y ADQUIRIÓ "
                         "objetivo (amIdle=0 + Task_MeleeAttack o currentTarget!=0) atacando a la tanda de "
                         "{} NPC(s). El encolado del melee FUNCIONA y el AI tick lo procesó.",
                         s_autotestTargetCount);
            core.GetNativeHud().AddSystemMessage("Auto-test: COMBATE DESBLOQUEADO (host ataca)");
            s_autotestDone.store(true);
            return;
        }
        // Caso raro: adquirió objetivo pero el motor aún marca amIdle=1 (transitorio). Lo tratamos
        // como positivo igualmente (hay objetivo/melee) pero lo dejamos anotado en el log.
        // EN: Rare case: target acquired but the engine still marks amIdle=1 (transient). Treated as positive
        //     anyway (there is a target/melee) but noted in the log.
        if (meleeNow || hasTarget) {
            const char* meleeKind = meleeAutoNow ? "automatico(Task_MeleeAttack)"
                                  : meleeFocusedNow ? "ordenado(Task_FocusedMeleeAttack)"
                                  : "sin-tarea-melee(solo currentTarget)";
            spdlog::info(
                "[AUTOTEST] POST OK (tick {} de la ventana, {} objetivos) host=0x{:X} | combat=0x{:X} "
                "currentTarget=0x{:X} | activeTask=0x{:X} vtblRVA=0x{:X} (melee={} tipo={}) | amIdle={} "
                "(objetivo adquirido aunque amIdle aún=1, transitorio)",
                s_autotestPostCounter, s_autotestTargetCount, s_autotestHostChar, post.combat,
                post.currentTarget, post.activeTask, activeTaskRva, meleeNow ? 1 : 0, meleeKind, post.amIdle);
            spdlog::info("[AUTOTEST] ==> *** COMBATE DESBLOQUEADO *** el host ADQUIRIÓ objetivo "
                         "(Task_MeleeAttack o currentTarget!=0) sobre la tanda de {} NPC(s).",
                         s_autotestTargetCount);
            core.GetNativeHud().AddSystemMessage("Auto-test: COMBATE DESBLOQUEADO (host ataca)");
            s_autotestDone.store(true);
            return;
        }

        // Aún sin objetivo: ¿queda ventana? Loguear progreso cada ~30 ticks y seguir muestreando.
        // EN: No target yet: any window left? Log progress every ~30 ticks and keep sampling.
        if (s_autotestPostCounter < kAutotestPostWindow) {
            if (s_autotestPostCounter == kAutotestPostMinTicks ||
                (s_autotestPostCounter % 30) == 0) {
                spdlog::info(
                    "[AUTOTEST] POST esperando adquisición (tick {}/{}) host=0x{:X} | combat=0x{:X} "
                    "currentTarget=0x{:X} activeTask=0x{:X} vtblRVA=0x{:X} amIdle={} (attackTarget solo "
                    "encola; el AI tick aún no ha consumido la orden — normal en los primeros ticks)",
                    s_autotestPostCounter, kAutotestPostWindow, s_autotestHostChar, post.combat,
                    post.currentTarget, post.activeTask, activeTaskRva, post.amIdle);
            }
            return;  // seguir muestreando
        }

        // Ventana agotada SIN adquirir objetivo → veredicto NEGATIVO con diagnóstico afinado.
        // EN: Window exhausted WITHOUT acquiring a target → NEGATIVE verdict with refined diagnosis (no
        //     CombatClass / host idle / host busy with another task).
        const char* verdict;
        if (post.combat == 0) {
            verdict = "*** SIN CombatClass: el host no tiene capa de combate (*(CharBody+0x8)=0). "
                      "attackTarget no puede materializar ataque. Revisar FIX-COMBATCLASS. ***";
        } else if (post.amIdle == 1) {
            verdict = "*** HOST OCIOSO con todos los objetivos de la tanda tras toda la ventana: las "
                      "órdenes se encolaron pero el AI tick NO las consumió (gate GW+0x8B9 paused, AI "
                      "tick no corre, o orden rechazada en pushOrder 0x674300). Ninguna orden de ataque "
                      "llega a Task_MeleeAttack. ***";
        } else {
            verdict = "*** HOST OCUPADO con OTRA tarea (no melee) toda la ventana: el AI tick procesa "
                      "otra cosa (Job/mover/idle-animado) en vez de la orden de ataque, o los gates "
                      "isAlly del encolador 0x6744A0 la rechazaron (objetivo visto como aliado). ***";
        }
        spdlog::info(
            "[AUTOTEST] POST FINAL (ventana {} ticks agotada, {} objetivos atacados) host=0x{:X} | "
            "combat=0x{:X} currentTarget=0x{:X} | activeTask=0x{:X} vtblRVA=0x{:X} (≠Task_MeleeAttack "
            "0x16BE448; ≠Task_FocusedMeleeAttack 0x16BF9E8; ≠Tasker 0x16BDC68) | amIdle={}",
            kAutotestPostWindow, s_autotestTargetCount, s_autotestHostChar, post.combat,
            post.currentTarget, post.activeTask, activeTaskRva, post.amIdle);
        spdlog::info("[AUTOTEST] ==> {}", verdict);
        core.GetNativeHud().AddSystemMessage("Auto-test de combate completado (ver log [AUTOTEST])");
        s_autotestDone.store(true);
        return;
    }

    // ── FASE 1: buscar NPC enemigo cercano y DISPARAR attackTarget ──
    // EN: ── PHASE 1: find a nearby enemy NPC and FIRE attackTarget ──
    if (s_autotestAttempts >= AUTOTEST_MAX_ATTEMPTS) {
        // Rendición silenciosa: nunca apareció un enemigo cercano (Zero no se acercó a nadie).
        // EN: Quiet give-up: no nearby enemy ever appeared (logged only once).
        if (s_autotestAttempts == AUTOTEST_MAX_ATTEMPTS) {
            spdlog::info("[AUTOTEST] sin NPC enemigo cercano tras {} intentos — auto-test en espera "
                         "(acércate a un grupo hostil para dispararlo).", s_autotestAttempts);
            s_autotestAttempts++;  // que el log salga una sola vez
        }
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastAutotestTry < std::chrono::milliseconds(250)) return;
    s_lastAutotestTry = now;
    s_autotestAttempts++;

    // GameWorld resuelto (mismo método que FIX-CONTROL / FIX-HOSTILITY).
    // EN: Resolved GameWorld (same method as FIX-CONTROL / FIX-HOSTILITY).
    game::GameWorldAccessor world(game::GetResolvedGameWorld());
    uintptr_t gwObj = world.IsValid() ? world.GetWorldObject() : 0;
    if (gwObj == 0) return;

    // char primario del host (mismo helper que el resto del mod / los DIAG).
    // EN: Host primary char (same helper as the rest of the mod / the DIAGs).
    uintptr_t hostChar = game::GetPlayerPrimaryCharacterDirect();
    if (!isHeap(hostChar)) return;   // char aún no resoluble → reintentar

    // Facción del host: PRIMARIA = GetPlayerFactionDirect; FALLBACK = char+0x10.
    // EN: Host faction: PRIMARY = GetPlayerFactionDirect; FALLBACK = char+0x10.
    uintptr_t hostFaction = game::GetPlayerFactionDirect();
    if (!isHeap(hostFaction)) {
        if (!Memory::Read(hostChar + 0x10, hostFaction) || !isHeap(hostFaction)) return;
    }

    // ES: RVAs nativas: attackTarget = 0x5CB0A0, isEnemy (FactionRelations) = 0x6B26D0.
    // EN: Native RVAs: attackTarget = 0x5CB0A0, isEnemy (FactionRelations) = 0x6B26D0.
    AttackTargetFn attackFn = reinterpret_cast<AttackTargetFn>(modBase + 0x5CB0A0);
    IsEnemyFn      isEnemyFn = reinterpret_cast<IsEnemyFn>(modBase + 0x6B26D0);

    AutotestSnapshot snap{};
    SEH_RunAutotest(modBase, gwObj, hostChar, hostFaction, attackFn, isEnemyFn,
                    kAutotestRadius, &snap);

    if (snap.targetCount == 0) {
        // No hay NINGÚN NPC de facción distinta cercano todavía. Log esporádico para no spamear.
        // Informamos cuántos candidatos (facción != host) vio el barrido (0 = Zero aún no está
        // cerca de ningún NPC de otra facción).
        // EN: No NPC of a different faction nearby yet. Sporadic log to avoid spam, reporting how many
        //     candidates (faction != host) the sweep saw.
        if (s_autotestAttempts <= 3 || s_autotestAttempts % 80 == 0) {
            spdlog::info("[AUTOTEST] buscando NPCs CERCANOS de CUALQUIER facción distinta (char+0x10 != "
                         "host, radio {:.0f}) — host=0x{:X} faction=0x{:X} simlistRecorrida={} "
                         "candidatosVistos={} (intento {}). Acércate a una zona con gente.",
                         kAutotestRadius, hostChar, hostFaction, snap.scanned, snap.banditsSeen,
                         s_autotestAttempts);
        }
        return;
    }

    // Excepción durante la tanda (señalada en targets[0].attackCalled = -1) → reintentar sin marcar
    // fired (no spamear). Solo si NO se disparó ninguno; si alguno se disparó OK seguimos al POST.
    // EN: Exception during the batch (signaled by targets[0].attackCalled = -1) → retry without marking
    //     fired. Only if NONE fired; if some fired OK we move on to POST.
    if (snap.targets[0].attackCalled == -1 && snap.attackedCount == 0) {
        if (s_autotestAttempts <= 5 || s_autotestAttempts % 60 == 0) {
            spdlog::warn("[AUTOTEST] excepción al buscar/atacar la tanda (host=0x{:X}) — reintento {}.",
                         snap.hostChar, s_autotestAttempts);
        }
        return;
    }

    // Se encontró una TANDA de NPCs y se disparó attackTarget a cada uno → log de la tanda + PRE.
    // preMelee = el host YA estaba en melee antes de la orden, sea automático (Task_MeleeAttack) u
    // ordenado (Task_FocusedMeleeAttack, clic manual previo). Solo informativo del estado PRE.
    // EN: A BATCH was found and attackTarget fired at each one → log the batch + PRE. preMelee = the host was
    //     ALREADY in melee before the order (automatic or ordered). Informative only.
    bool preMelee = (snap.pre.activeTaskVtbl == taskMeleeVtblAbs ||
                     snap.pre.activeTaskVtbl == taskFocusedMeleeVtblAbs);
    spdlog::info(
        "[AUTOTEST] TANDA encontrada: {} NPC(s) de facción distinta en radio {:.0f} (cap {}) | "
        "host=0x{:X} faction=0x{:X} | hostTasker(char+0x658→+0x98)=0x{:X} | hostDefaultRel "
        "FR+0x60={:.1f} | candidatosVistos={} simlistRecorrida={}",
        snap.targetCount, kAutotestRadius, kAutotestMaxTargets, snap.hostChar, hostFaction,
        snap.hostTasker, snap.hostDefaultRel, snap.banditsSeen, snap.scanned);

    // Log POR CADA objetivo de la tanda: nombre, facción, distancia, hostilidad nativa y si se atacó.
    // EN: Log PER batch target: name, faction, distance, native hostility and whether it was attacked.
    for (int i = 0; i < snap.targetCount; ++i) {
        const AutotestTarget& t = snap.targets[i];
        const char* enemyStr = (t.hostEnemyNpc == 1) ? "SI" : (t.hostEnemyNpc == 0 ? "NO" : "??");
        const char* atkStr   = (t.attackCalled == 1) ? "INVOCADO" :
                               (t.attackCalled == -1 ? "EXCEPCION" : "NO");
        spdlog::info(
            "[AUTOTEST] objetivo {}/{}: npc='{}' (0x{:X}) faction=0x{:X} fundType={} (8=bandido,"
            "7=esclavista) dist={:.1f} | isEnemy NATIVO host->npc={} | attackTarget={}",
            i + 1, snap.targetCount, t.npcName, t.npcChar, t.npcFaction, t.npcFundType, t.npcDist,
            enemyStr, atkStr);
    }

    spdlog::info(
        "[AUTOTEST] PRE host=0x{:X} | combat=0x{:X} currentTarget=0x{:X} | activeTask=0x{:X} "
        "vtbl=0x{:X} (melee={}) | amIdle={} | attackTargetInvocados={}/{}",
        snap.hostChar, snap.pre.combat, snap.pre.currentTarget, snap.pre.activeTask,
        snap.pre.activeTaskVtbl, preMelee ? 1 : 0, snap.pre.amIdle, snap.attackedCount,
        snap.targetCount);

    if (snap.attackedCount > 0) {
        spdlog::info("[AUTOTEST] TANDA disparada: {} attackTarget(0x5CB0A0)(host=0x{:X}, npc) INVOCADOS "
                     "✓ (cada uno SOLO encola orden mode 4; el AI tick las materializará en ticks "
                     "posteriores) — muestreando POST del HOST hasta {} ticks.", snap.attackedCount,
                     snap.hostChar, kAutotestPostWindow);
        core.GetNativeHud().AddSystemMessage("Auto-test: tanda de ataques disparada (ver [AUTOTEST])");
        s_autotestHostChar    = snap.hostChar;
        s_autotestNpcChar     = snap.targets[0].npcChar;   // 1er objetivo (referencia)
        s_autotestPostCounter = 0;
        s_autotestTargetCount = snap.targetCount;          // guardado para el veredicto global
        s_autotestFired.store(true);   // → la próxima vez entramos en la FASE 2 (POST)
    }
}

// ── Helpers SEH para los DIAG/FIX de simulación (Fase 4) ────────────────────────
// IMPORTANTE: __try/__except NO puede convivir con objetos C++ con destructor en la
// misma función (error C2712 "unwinding"). OnGameTick tiene objetos C++ (accessors,
// spdlog), así que TODA lectura/escritura protegida con SEH se aísla en estos helpers
// POD (sin objetos C++ que requieran unwinding). Devuelven los datos por punteros.
// EN: ── SEH helpers for the simulation DIAGs/FIXes (Phase 4) ──
//     IMPORTANT: __try/__except cannot coexist with C++ objects with destructors in the same function
//     (error C2712 "unwinding"). OnGameTick has C++ objects (accessors, spdlog), so EVERY SEH-protected
//     read/write is isolated in these POD helpers. They return data through pointers.

// Vuelca el estado del round-robin de AI tick + guards de reentrancia (SOLO LECTURA).
// Estructura POD de salida para evitar objetos C++ dentro del __try.
// EN: Dumps the AI tick round-robin state + reentrancy guards (READ ONLY). POD output struct to avoid C++
//     objects inside the __try.
struct SimDiagSnapshot {
    // Contadores .data del round-robin del AI tick (updateCharacters 0x786E30).
    // ctrA/ctrB = 0x2132EC8/ECC (clases C/B, ramas +0xE0 update completo).
    // aiTickClassA = 0x2132ED0 (CLASE A — la que dispara [vtbl+0xE8] = combate/Jobs/
    //   levantarse/recuperar KO). Es el contador relevante del síntoma.
    // frameCtr = 0x2132ED4 (cursor de frame, reset a 0 cada pasada en 0x786EAA).
    // EN: .data counters of the AI tick round-robin (updateCharacters 0x786E30): ctrA/ctrB = 0x2132EC8/ECC
    //     (classes C/B), aiTickClassA = 0x2132ED0 (CLASS A — fires [vtbl+0xE8] = combat/Jobs/get up/recover
    //     from KO; the relevant counter for the symptom), frameCtr = 0x2132ED4 (frame cursor).
    uint32_t ctrA = 0, ctrB = 0, aiTickClassA = 0, frameCtr = 0;
    // GATE REAL de mainLoop (0x788FF5): si !=0, updateCharacters NO corre. ESTA es la causa.
    // EN: REAL mainLoop gate (0x788FF5): if !=0, updateCharacters does NOT run. GW+0x8B9 = paused flag,
    //     GW+0x700 = gameSpeed (0.0 re-sets the gate), GW+0x770 = active chars, char+0xE4 = host LOD.
    uint8_t  gatePause = 0xFF;    // GW+0x8B9 (flag paused)
    float    gameSpeed = -1.f;    // GW+0x700 (si ==0.0 exacto, el setter re-pega el gate)
    uint64_t activeCnt = (uint64_t)-1;  // GW+0x770 (nº chars activos)
    uint8_t  primaryLod = 0xFF;   // char+0xE4 del host (LOD: decide rama de update)

    // ── RELOJ DE SIMULACIÓN (Fase 4 — doble verificación de bytes 2026-06-18) ──────
    // El AI tick de combate (0x5CCD90, [vtbl+0xE8]) deriva su dt de un RELOJ DE
    // SIMULACIÓN que es un double en SimClock+0xA0, donde:
    //   SimClock = *(void**)(modBase + 0x21303D0)   (puntero a objeto, único escritor
    //              0x66E502; init pone +0xA0=0). NO es la instancia GameWorld embebida.
    //   reloj(+0xA0) = [SimClock+0x8](día int) * 24.0 + horaDelDía(float en [SimClock+0x20]+0x1C)
    //   → el reloj está en HORAS DE JUEGO acumuladas (día*24 + hora). RE confirmado:
    //     0x66CF96/0x66DCFB/0x67000A escriben +0xA0; const 0x1686480 = 24.0.
    //   El AI tick (0x5CCE36) hace cvtsd2ss xmm6,[reloj] y lo compara con char+0xD0
    //   (última hora procesada por ESE char) con umbrales 0.0 / 12.0 (0x16B3BFC=12.0).
    //   Si reloj NO avanza entre frames → diff(reloj - char+0xD0)≈0 → el AI tick no
    //   hace trabajo de combate/levantarse/recuperar-KO aunque corra. = síntoma Fase 4.
    // EN: ── SIMULATION CLOCK (Phase 4 — byte double-check 2026-06-18) ── The combat AI tick (0x5CCD90)
    //     derives its dt from a SIMULATION CLOCK, a double at SimClock+0xA0, where SimClock =
    //     *(void**)(modBase + 0x21303D0) (object pointer, single writer 0x66E502; NOT the embedded GameWorld).
    //     clock = day(int at SimClock+0x8) * 24.0 + hourOfDay(float at [SimClock+0x20]+0x1C) → accumulated
    //     GAME HOURS. The AI tick converts it to float and compares it with char+0xD0 (last hour processed
    //     by THAT char) with thresholds 0.0 / 12.0. If the clock does not advance, diff≈0 → no combat work.
    uintptr_t simClockPtr = 0;    // *(modBase+0x21303D0): puntero al objeto reloj
    double    simClock    = -1.0; // SimClock+0xA0: reloj de simulación (horas de juego)
    int32_t   simDay      = -1;   // SimClock+0x08: día (int) — componente entero del reloj
    float     simTimeOfDay= -1.f; // [SimClock+0x20]+0x1C: hora del día (float, fracción)
    // char+0xD0 = última hora procesada por el AI tick de ESTE char. ES UN FLOAT de 4 bytes:
    // el motor lo lee con `movss xmm3,[rdi+0xD0]` en 0x5CCE3A (RE verificado). Si se declara
    // como double y se lee con Memory::Read (8 bytes) se mezclan los 4 bytes del float con los
    // 4 bytes siguientes (otro campo) → sale un número gigante basura en el log [DIAG-CLOCK].
    // Por eso es FLOAT, no double. (Fix punto 5 — 2026-06-18.)
    // EN: char+0xD0 = last hour processed by THIS char AI tick. It is a 4-byte FLOAT (read with movss at
    //     0x5CCE3A). Reading it as a double (8 bytes) mixes in the next field → garbage in [DIAG-CLOCK].
    float     primaryLastT= -1.f; // char+0xD0 del host: última hora procesada por el AI tick (FLOAT)

    // ── LISTA DE SIMULACIÓN ACTIVA (Fase 4 — DIAG-SIMLIST, discrimina H1) ──────────
    // updateCharacters (0x786E30) NO itera el squad del mod (GW+0x580→+0x2B0); itera la
    // LISTA DE SIMULACIÓN derivada del hash set GW+0x750:
    //   array = [GW+0x788] (array de nodos), idx = [GW+0x768] (head), count = [GW+0x770].
    //   head  = array[idx]; nodo: +0x00 = next, +0x10 = Character*.
    // H1 (la más probable): el char primario del host NO está en esta lista → updateCharacters
    // nunca lo recorre → el AI tick [vtbl+0xE8] jamás lo toca, aunque el reloj avance y el
    // squad sea correcto. ESTOS campos confirman/refutan H1 SIN escribir nada (read-only).
    //   hostInSimList:  0 = NO está, 1 = SÍ está, -1 = no se pudo recorrer (lista no legible).
    //   simListCount:   GW+0x770 (nº de chars en la lista de simulación; ojo, != tamaño squad).
    //   simListWalked:  nodos efectivamente recorridos (para detectar lista corrupta / cap).
    // EN: ── ACTIVE SIMULATION LIST (Phase 4 — DIAG-SIMLIST, discriminates H1) ── updateCharacters does NOT
    //     iterate the mod squad (GW+0x580→+0x2B0); it iterates the SIMULATION LIST derived from the hash set
    //     GW+0x750: array=[GW+0x788], idx=[GW+0x768] (head), count=[GW+0x770]; node +0x00 = next,
    //     +0x10 = Character*. H1: the host primary char is NOT in this list → the AI tick never touches it.
    //     These fields confirm/refute H1 read-only (hostInSimList: 0 no, 1 yes, -1 unknown).
    int       hostInSimList = -1;   // -1 = no determinable
    uint64_t  simListCount  = (uint64_t)-1; // GW+0x770 (renombrado vs activeCnt para el log)
    uint32_t  simListWalked = 0;    // nodos visitados durante el recorrido

    // ── GATES DEL AI TICK 0x5CCD90 (Fase 4 — DIAG-AICHK) ───────────────────────────
    // CORRECCIÓN 2026-06-18: el comentario anterior interpretaba +0x5BC AL REVÉS. Triple
    // verificación RE independiente (iced-x86 sobre Steam 1.0.68) demuestra:
    //   char+0x5BC = FLAG "MUERTO" (isDead). 0 = VIVO, 1 = MUERTO.
    //   • Único setter en TODO .text: 0x7A6242 `mov byte[rcx+0x5BC],1`, dentro de la rutina
    //     de muerte 0x7A6200 (strings "has died from blood loss/starvation", "is dead").
    //   • Getter dedicado bool Character::isDead() en 0x6215B0.
    // Desensamblado real del gate:
    //   0x5CCDDF  xor esi,esi                 ; sil = 0
    //   0x5CCE24  cmp byte [rdi+0x5BC], sil   ; compara flag-muerto contra 0
    //   0x5CCE2B  je  0x5CD1C0                ; si +0x5BC==0 (VIVO) → SALTA a 0x5CD1C0
    //                                         ;   = RAMA DEL CHAR VIVO (reintegra al GameWorld
    //                                         ;   0x2134110, llama vtable de IA: think 0x1D8,
    //                                         ;   commit acción a GameWorld 0xA0AF10). El umbral
    //                                         ;   0.75 (char+0xD8) vive AQUÍ, en la rama viva.
    //   0x5CCE31… (NO salta, +0x5BC!=0/MUERTO) → rama CADÁVER: catch-up de horas de simulación,
    //             usa +0xD0 (FLOAT) con umbrales 6.0/12.0 = decay/limpieza del cuerpo.
    //   0x5CCE36  cvtsd2ss xmm6,[rax]         ; reloj de sim (double→float)
    //   0x5CCE3A  movss xmm3,[rdi+0xD0]       ; +0xD0 es FLOAT (movss). SOLO se usa en cadáver.
    //   0x5CCE57  cmp byte  [rdi+0x3D4], sil  ; +0x3D4 BYTE   (rama cadáver)
    //   0x5CCE60  cmp dword [rdi+0x2F8], esi  ; +0x2F8 DWORD  (rama cadáver)
    //   0x5CCE6F  comiss (reloj-char+0xD0), 12.0
    // CONSECUENCIA CRÍTICA PARA EL FIX: un char VIVO (+0x5BC==0) salta a 0x5CD1C0 ANTES de
    // tocar +0xD0 → el seed de char+0xD0 SOLO afecta a CADÁVERES, NO a chars vivos. El combate
    // congelado del host (char vivo) NO se arregla ni con +0x5BC ni con el seed de +0xD0: la
    // causa raíz está aguas arriba (gate GW+0x8B9 en mainLoop 0x788FF5 / el char no entra en
    // updateCharacters 0x786E30). ESCRIBIR +0x5BC=1 MATARÍA al char (lo manda a rama cadáver y
    // ~50 sitios lo tratarían como fiambre). NO TOCAR +0x5BC. Estos campos son SOLO diagnóstico.
    // EN: ── GATES OF AI TICK 0x5CCD90 (Phase 4 — DIAG-AICHK) ── CORRECTION 2026-06-18: char+0x5BC is the
    //     "DEAD" flag (isDead): 0 = ALIVE, 1 = DEAD (only setter 0x7A6242 inside the death routine 0x7A6200;
    //     getter Character::isDead() at 0x6215B0). An ALIVE char jumps to 0x5CD1C0 (live branch: think,
    //     commit to GameWorld; the 0.75 threshold on char+0xD8 lives here). Only DEAD chars take the corpse
    //     branch that uses +0xD0 (FLOAT) with thresholds 6.0/12.0 (decay/cleanup) and +0x3D4 / +0x2F8.
    //     CRITICAL CONSEQUENCE: the char+0xD0 seed ONLY affects CORPSES, not living chars; the host frozen
    //     combat is not fixed by +0x5BC nor by the +0xD0 seed. WRITING +0x5BC=1 WOULD KILL the char. DO NOT
    //     TOUCH +0x5BC. These fields are diagnostic only.
    //     NOTE (inconsistency): this contradicts the [FIX-SIMSEED] header above, which still presents the
    //     +0xD0 seed as the "confirmed root cause" of the frozen combat.
    // ES: NOTA (incoherencia): esto contradice la cabecera de [FIX-SIMSEED] de más arriba, que sigue
    //     presentando la siembra de +0xD0 como "causa raíz confirmada" del combate congelado.
    uint8_t   aiGate5BC = 0xFF;  // char+0x5BC (byte) — FLAG MUERTO; 0=VIVO (→0x5CD1C0), 1=MUERTO
    uint8_t   aiGate3D4 = 0xFF;  // char+0x3D4 (byte) — usado SOLO en rama cadáver (0x5CCE57)
    int32_t   aiGate2F8 = -1;    // char+0x2F8 (dword)— usado SOLO en rama cadáver (0x5CCE60)

    // ── GATE INTERNO DE LA RAMA VIVA 0x5CD1C0 (DIAG-THINK, discrimina H_B) ──────────
    // Para un char VIVO, el "think" pesado (decidir acción/combate, vtable 0x1D8/0x1E0 +
    // commit a GameWorld 0xA0AF10) SOLO corre si el timer char+0xD8 supera 0.75 (comiss en
    // 0x5CD1EC) Y el flag char+0xDC != 0 (gate en 0x5CD1CD). Si +0xD8 nunca acumula > 0.75
    // (porque el dt de simulación que llega es 0 — sync Fase 4) el char vivo entra a la rama
    // viva pero hace early-skip → solo update de movimiento, nunca piensa/ataca. Mismo
    // síntoma que H_A (no entrar) pero causa distinta. Estos campos lo discriminan. SOLO lectura.
    // EN: ── INNER GATE OF THE LIVE BRANCH 0x5CD1C0 (DIAG-THINK, discriminates H_B) ── For an ALIVE char the
    //     heavy "think" only runs depending on the char+0xD8 value (compared against 0.75 at 0x5CD1EC) AND
    //     the char+0xDC flag != 0 (gate at 0x5CD1CD). READ ONLY. (See the 2026-06-18 correction in
    //     SEH_ReadSimDiag: the comparison direction described here is the old, inverted reading.)
    // ES: (Ver la corrección del 2026-06-18 en SEH_ReadSimDiag: el sentido de la comparación descrito aquí
    //     es la lectura antigua, invertida.)
    float     thinkTimerD8 = -1.f; // char+0xD8 (float) — timer del think pesado; gate vs 0.75
    uint8_t   thinkFlagDC  = 0xFF; // char+0xDC (byte)  — !=0 requerido para pensar (0x5CD1CD)
    int       thinkRead    = 0;    // 1 si se leyeron ambos bajo SEH
    int       aiGateRead = 0;    // 1 si se leyeron los tres gates bajo SEH; 0 si no (char null/AV)
};

// ── PrimaryDiagSnapshot (Fase 4 — DIAG-PRIMARY, ¿char correcto vs equivocado?) ──────
// PROPÓSITO: confirmar en runtime si el mod resuelve el char REALMENTE controlado por el
// jugador, o uno equivocado. Hallazgo de RE (game-reverse-engineer, verificado por bytes
// 2026-06-18 sobre Steam 1.0.68):
//   • El mod (GetPlayerPrimaryCharacterDirect / EntityRegistry) usa SIEMPRE el ELEMENTO 0
//     del lektor playerCharacters en PlayerInterface+0x2B0 (data[0]).
//   • PERO el MOTOR guarda el char "controlado/activo" en un campo SEPARADO:
//       PlayerInterface+0x2A8 = controlledChar (Character*).
//     Confirmado por desensamblado de SetControlledChar (RVA 0x802520, string "Player now
//     controlling: " en 0x171AA88):
//       0x80267A  mov ecx,[r8+0x210]      ; r8=Faction(PI+0x2A0); ecx = memberCount
//       0x802681  dec ecx                 ; index = count-1  (ÚLTIMO miembro)
//       0x802683  mov rax,[r8+0x218]      ; memberArray (Character**)
//       0x80268A  mov rcx,[rax+rcx*8]     ; data[count-1]
//       0x80268E  mov [rbx+0x2A8],rcx     ; PI+0x2A8 = char controlado
//     Y el consumo (aplica la acción del jugador) en 0x50E9CF: mov rcx,[rcx+0x2A8].
//   ⇒ Si PI+0x2A8 != PI+0x2B0 data[0], el mod aplica sus fixes (faction +0x10, seed +0xD0)
//     al char EQUIVOCADO y el char real del jugador nunca se toca = causa raíz candidata.
// Este snapshot vuelca AMBOS chars + la lista de facción para comparar. SOLO LECTURA, SEH.
// POD puro (sin objetos C++) para poder vivir dentro de un __try (restricción C2712).
// EN: ── PrimaryDiagSnapshot (Phase 4 — DIAG-PRIMARY: right char vs wrong char?) ── PURPOSE: confirm at
//     runtime whether the mod resolves the char REALLY controlled by the player, or a wrong one. RE
//     finding (verified by bytes 2026-06-18, Steam 1.0.68): the mod (GetPlayerPrimaryCharacterDirect /
//     EntityRegistry) ALWAYS uses ELEMENT 0 of the playerCharacters lektor at PlayerInterface+0x2B0
//     (data[0]), BUT the ENGINE stores the "controlled/active" char in a SEPARATE field:
//     PlayerInterface+0x2A8 = controlledChar, which SetControlledChar (RVA 0x802520) fills with
//     Faction.members[count-1] (see listing above) and which is consumed at 0x50E9CF. If PI+0x2A8 !=
//     data[0], the mod applies its fixes to the WRONG char = candidate root cause. This snapshot dumps
//     BOTH chars + the faction list to compare. READ ONLY, SEH, pure POD (C2712).
struct PrimaryDiagSnapshot {
    int       resolved      = 0;   // 1 si se pudo resolver PlayerInterface; 0 si no
    uintptr_t playerIface   = 0;   // PI = *(GW+0x580)

    // (1) Lo que usa el MOD: lektor PI+0x2B0 → data[0].
    // EN: (1) What the MOD uses: lektor PI+0x2B0 → data[0] (size PI+0x2B8, capacity PI+0x2BC, name at
    //     char+0x18, faction at char+0x10).
    uint32_t  lektorSize    = 0;   // PI+0x2B8 (size del lektor playerCharacters)
    uint32_t  lektorCap     = 0;   // PI+0x2BC (capacity)
    uintptr_t modChar       = 0;   // data[0] (lo que agarra el mod)
    char      modName[32]   = {0}; // name(+0x18) del modChar (SSO/heap leído seguro)
    uintptr_t modFaction    = 0;   // modChar+0x10

    // (2) Lo que usa el MOTOR: PI+0x2A8 (controlledChar).
    // EN: (2) What the ENGINE uses: PI+0x2A8 (controlledChar).
    uintptr_t ctrlChar      = 0;   // PI+0x2A8 (char controlado real)
    char      ctrlName[32]  = {0}; // name(+0x18) del ctrlChar
    uintptr_t ctrlFaction   = 0;   // ctrlChar+0x10

    // (3) La fuente del motor: Faction(PI+0x2A0) → +0x210 count / +0x218 array → data[count-1].
    // EN: (3) The engine source: Faction(PI+0x2A0) → +0x210 count / +0x218 array → data[count-1].
    uintptr_t faction       = 0;   // PI+0x2A0 (participant / facción del jugador)
    uint32_t  facMemberCnt  = 0;   // Faction+0x210 (memberCount)
    uintptr_t facLastChar   = 0;   // Faction+0x218[count-1] (lo que SetControlledChar elige por defecto)

    // Veredictos de coincidencia (calculados en el helper, no en el __try del log).
    // EN: Match verdicts (computed in the helper, not in the logging __try).
    int       modEqCtrl     = -1;  // 1 = modChar==ctrlChar, 0 = distintos, -1 = no comparable
    int       modEqFacLast  = -1;  // 1 = modChar==facLastChar, 0 = distintos, -1 = no comparable
    int       ctrlEqFacLast = -1;  // 1 = ctrlChar==facLastChar, 0 = distintos, -1 = no comparable
};

// Recorre la LISTA DE SIMULACIÓN activa (GW+0x768/+0x770/+0x788) EXACTAMENTE como lo hace
// updateCharacters (0x786E30, ver audit-06 §6) y comprueba si primaryChar está en ella.
// SOLO LECTURA. Cada Memory::Read ya está protegido con SEH, pero envolvemos todo el
// recorrido en un __try adicional (POD, sin objetos C++) por defensa ante nodos corruptos.
// No escribe NADA en estructuras del motor (el fix de H1 NO se aplica aquí).
// EN: Walks the ACTIVE SIMULATION LIST (GW+0x768/+0x770/+0x788) EXACTLY like updateCharacters
//     (0x786E30, see audit-06 §6) and checks whether primaryChar is in it. READ ONLY. Every Memory::Read
//     is already SEH-protected, but the whole walk is wrapped in an extra __try (POD) against corrupted
//     nodes. Writes NOTHING to engine structures (the H1 fix is not applied here).
static void SEH_WalkSimList(uintptr_t gwObj, void* primaryChar, SimDiagSnapshot* out) {
    if (gwObj == 0) return;  // sin GameWorld no hay lista que recorrer
    __try {
        // count = [GW+0x770]: si 0 → lista vacía, el host NO puede estar en ella.
        // EN: count = [GW+0x770]: if 0 → empty list, the host cannot be in it.
        uint64_t count = 0;
        if (!Memory::Read(gwObj + 0x770, count)) { out->hostInSimList = -1; return; }
        out->simListCount = count;
        if (count == 0) { out->hostInSimList = 0; return; }  // lista vacía → host fuera

        // array = [GW+0x788] (puntero al array de nodos). idx = [GW+0x768] (head index).
        // EN: array = [GW+0x788] (pointer to the node array). idx = [GW+0x768] (head index).
        uintptr_t arrayPtr = 0;
        uintptr_t headIdx  = 0;
        if (!Memory::Read(gwObj + 0x788, arrayPtr) || arrayPtr <= 0x10000) {
            out->hostInSimList = -1; return;   // array no plausible
        }
        if (!Memory::Read(gwObj + 0x768, headIdx)) { out->hostInSimList = -1; return; }

        // head = array[idx] → lectura del slot array + idx*8 (punteros de 8 bytes en x64).
        // EN: head = array[idx] → read slot array + idx*8 (8-byte pointers on x64).
        uintptr_t node = 0;
        if (!Memory::Read(arrayPtr + headIdx * 8, node)) { out->hostInSimList = -1; return; }

        // Recorrido de la lista enlazada: nodo+0x10 = Character*, nodo+0x00 = next.
        // Cap defensivo de iteraciones (como CharacterIterator) para no colgarse si la
        // lista está corrupta / es cíclica. 20000 >> cualquier nº realista de chars.
        // EN: Linked-list walk: node+0x10 = Character*, node+0x00 = next. Defensive iteration cap (like
        //     CharacterIterator) so a corrupted / cyclic list cannot hang us. 20000 >> any realistic char count.
        const uint32_t kMaxIter = 20000;
        const uintptr_t wantedChar = reinterpret_cast<uintptr_t>(primaryChar);
        int found = 0;
        uint32_t walked = 0;
        while (node > 0x10000 && walked < kMaxIter) {
            walked++;
            uintptr_t chr = 0;
            if (Memory::Read(node + 0x10, chr)) {
                if (wantedChar != 0 && chr == wantedChar) { found = 1; break; }
            }
            uintptr_t next = 0;
            if (!Memory::Read(node + 0x00, next)) break;  // nodo ilegible → paramos
            if (next == node) break;                      // auto-ciclo trivial → paramos
            node = next;
        }
        out->simListWalked = walked;
        // Si primaryChar es null no podemos afirmar nada → dejamos -1.
        out->hostInSimList = (wantedChar == 0) ? -1 : found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->hostInSimList = -1;  // cualquier AV imprevisto → "no determinable"
    }
}
// ES: Lee el diagnóstico de simulación (SOLO LECTURA, SEH): contadores del round-robin del AI tick en
//     .data (RVAs 0x2132EC8..0x2132ED4), gate de pausa GW+0x8B9, gameSpeed GW+0x700, nº de chars activos
//     GW+0x770, y del char primario: LOD (+0xE4), última hora procesada (+0xD0), gates del AI tick
//     (+0x5BC isDead, +0x3D4, +0x2F8), think (+0xD8/+0xDC) y el reloj de simulación.
// EN: Reads the simulation diagnostic (READ ONLY, SEH): AI tick round-robin counters in .data (RVAs
//     0x2132EC8..0x2132ED4), pause gate GW+0x8B9, gameSpeed GW+0x700, active char count GW+0x770, and for
//     the primary char: LOD (+0xE4), last processed hour (+0xD0), AI tick gates (+0x5BC isDead, +0x3D4,
//     +0x2F8), think (+0xD8/+0xDC) and the simulation clock.
static void SEH_ReadSimDiag(uintptr_t modBase, uintptr_t gwObj, void* primaryChar,
                            SimDiagSnapshot* out) {
    __try {
        Memory::Read(modBase + 0x2132EC8, out->ctrA);          // clase C
        Memory::Read(modBase + 0x2132ECC, out->ctrB);          // clase B
        Memory::Read(modBase + 0x2132ED0, out->aiTickClassA);  // CLASE A (AI tick combate)
        Memory::Read(modBase + 0x2132ED4, out->frameCtr);      // cursor de frame
        if (gwObj != 0) {
            Memory::Read(gwObj + 0x770, out->activeCnt);   // nº chars activos
            Memory::Read(gwObj + 0x8B9, out->gatePause);   // GATE real de mainLoop
            Memory::Read(gwObj + 0x700, out->gameSpeed);   // gameSpeed (re-pega el gate si ==0)
        }
        if (primaryChar != nullptr) {
            uintptr_t pc = reinterpret_cast<uintptr_t>(primaryChar);
            Memory::Read(pc + 0xE4, out->primaryLod);
            // char+0xD0: última hora de simulación procesada por el AI tick de ESTE char.
            // EN: char+0xD0: last simulation hour processed by THIS char AI tick.
            Memory::Read(pc + 0xD0, out->primaryLastT);

            // ── Gates del AI tick 0x5CCD90 (DIAG-AICHK) — SOLO LECTURA ──
            // Tamaños tomados del desensamblado real (ver SimDiagSnapshot):
            //   +0x5BC = byte (cmp byte), +0x3D4 = byte (cmp byte), +0x2F8 = dword (cmp dword).
            // EN: ── AI tick 0x5CCD90 gates (DIAG-AICHK) — READ ONLY ── Sizes from the real disassembly:
            //     +0x5BC = byte, +0x3D4 = byte, +0x2F8 = dword.
            bool g1 = Memory::Read(pc + 0x5BC, out->aiGate5BC);
            bool g2 = Memory::Read(pc + 0x3D4, out->aiGate3D4);
            bool g3 = Memory::Read(pc + 0x2F8, out->aiGate2F8);
            out->aiGateRead = (g1 && g2 && g3) ? 1 : 0;

            // Gate interno de la rama VIVA 0x5CD1C0 (DIAG-THINK): valor +0xD8 (float) y
            // flag +0xDC (byte).
            // CORRECCIÓN 2026-06-18 (RE de bytes, audit-08): char+0xD8 NO es un acumulador de
            // dt ni un timer que deba "cruzar" 0.75. Es una CACHÉ por-char de un valor derivado
            // de la HORA del juego (reloj global *(modBase+0x21303D0)), recalculado ENTERO cada
            // AI tick por 0x66CB50(reloj) en 0x5CD1C5 y por 0xA0AF10 en 0x5CD259. El gate real
            // (0x5CD1F4: comiss 0.75,[+0xD8]; jbe salir) corre el think pesado SOLO si +0xD8 <
            // 0.75 (semántica INVERSA a la documentada antes). Y el gate se SALTA por completo
            // si [vtbl+0x58]→+0x250 != 0 (jne 0x5CD1FD): piensa igualmente. Por tanto +0xD8 NO
            // bloquea el combate del host; si no pelea, la causa es aguas arriba (no recibe el
            // AI tick). Este DIAG se mantiene solo como evidencia del valor real de +0xD8/+0xDC.
            // EN: Inner gate of the LIVE branch 0x5CD1C0 (DIAG-THINK): +0xD8 value (float) and +0xDC flag (byte).
            //     CORRECTION 2026-06-18 (byte RE, audit-08): char+0xD8 is NOT a dt accumulator nor a timer that must
            //     "cross" 0.75. It is a per-char CACHE of a value derived from the game HOUR (global clock
            //     *(modBase+0x21303D0)), fully recomputed every AI tick (0x66CB50 at 0x5CD1C5 and 0xA0AF10 at
            //     0x5CD259). The real gate (0x5CD1F4: comiss 0.75,[+0xD8]; jbe exit) runs the heavy think ONLY if
            //     +0xD8 < 0.75 (INVERSE of what was documented before). And the gate is skipped entirely if
            //     [vtbl+0x58]→+0x250 != 0: it thinks anyway. So +0xD8 does NOT block host combat; if it does not
            //     fight, the cause is upstream (it does not receive the AI tick). Kept only as evidence.
            bool t1 = Memory::Read(pc + 0xD8, out->thinkTimerD8);
            bool t2 = Memory::Read(pc + 0xDC, out->thinkFlagDC);
            out->thinkRead = (t1 && t2) ? 1 : 0;
        }

        // ── Reloj de simulación: deref del puntero SimClock y lectura del reloj ──
        // *(modBase+0x21303D0) = objeto reloj. Validamos que sea un puntero plausible
        // (no NULL, no dentro del módulo) antes de leer sus campos, para no provocar AV.
        // EN: ── Simulation clock: deref the SimClock pointer and read the clock ── *(modBase+0x21303D0) = clock
        //     object. It is validated as a plausible pointer (non-NULL, not inside the module) before reading its
        //     fields, to avoid an AV.
        uintptr_t scPtr = 0;
        if (Memory::Read(modBase + 0x21303D0, scPtr) && scPtr > 0x10000) {
            out->simClockPtr = scPtr;
            Memory::Read(scPtr + 0xA0, out->simClock);   // double reloj (horas de juego)
            Memory::Read(scPtr + 0x08, out->simDay);     // día (int)
            uintptr_t clockState = 0;                    // [SimClock+0x20] → +0x1C = hora del día
            if (Memory::Read(scPtr + 0x20, clockState) && clockState > 0x10000)
                Memory::Read(clockState + 0x1C, out->simTimeOfDay);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Lee el name (std::string MSVC x64) de un Character en char+0x18 a un buffer C, SEH-safe.
// Layout std::string MSVC: +0x00 buf[16] (SSO) · +0x10 size (u64) · +0x18 capacity (u64).
// Si capacity>15 → los primeros 8 bytes de +0x00 son un heap-ptr a los datos. POD, sin C++.
// dst debe tener al menos 32 bytes. Trunca a 31 chars + NUL. Solo lectura.
// EN: Reads the name (MSVC x64 std::string) of a Character at char+0x18 into a C buffer, SEH-safe.
//     MSVC std::string layout: +0x00 buf[16] (SSO) · +0x10 size (u64) · +0x18 capacity (u64). If
//     capacity>15 the first 8 bytes at +0x00 are a heap pointer to the data. POD, no C++. dst must have
//     at least 32 bytes. Truncates to 31 chars + NUL, printable chars only. Read only.
static void SEH_ReadCharName(uintptr_t chrPtr, char* dst, size_t dstSize) {
    if (dst == nullptr || dstSize == 0) return;
    dst[0] = '\0';
    if (chrPtr <= 0x10000) return;
    __try {
        uintptr_t strBase = chrPtr + 0x18;       // char+0x18 = std::string name
        uint64_t  size = 0, cap = 0;
        if (!Memory::Read(strBase + 0x10, size)) return;
        Memory::Read(strBase + 0x18, cap);
        uintptr_t srcPtr;
        if (cap > 15) {                          // heap: primeros 8 bytes = puntero a datos
            if (!Memory::Read(strBase, srcPtr) || srcPtr <= 0x10000) return;
        } else {
            srcPtr = strBase;                    // SSO: los datos están inline
        }
        size_t n = (size < dstSize - 1) ? (size_t)size : dstSize - 1;
        for (size_t i = 0; i < n; ++i) {
            uint8_t c = 0;
            if (!Memory::Read(srcPtr + i, c)) { dst[i] = '\0'; return; }
            dst[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';  // solo imprimibles
        }
        dst[n] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) { dst[0] = '\0'; }
}

// ── SEH_ReadPrimaryDiag (DIAG-PRIMARY) — ¿char correcto vs equivocado? SOLO LECTURA ──
// Resuelve PlayerInterface (GW+0x580) y vuelca, bajo SEH, los TRES candidatos a "char del
// jugador" para compararlos:
//   (1) data[0] del lektor PI+0x2B0   (lo que usa el MOD hoy)
//   (2) PI+0x2A8 controlledChar       (lo que usa el MOTOR para las acciones del jugador)
//   (3) Faction(PI+0x2A0)+0x218[count-1]  (la fuente por defecto de SetControlledChar)
// NO escribe nada en el motor. Valida cada puntero como heap-ptr del juego antes de leerlo.
// EN: ── SEH_ReadPrimaryDiag (DIAG-PRIMARY) — right char vs wrong char? READ ONLY ── Resolves the
//     PlayerInterface (GW+0x580) and dumps, under SEH, the THREE "player char" candidates to compare:
//     (1) data[0] of the PI+0x2B0 lektor (what the MOD uses today); (2) PI+0x2A8 controlledChar (what the
//     ENGINE uses for player actions); (3) Faction(PI+0x2A0)+0x218[count-1] (SetControlledChar default
//     source). Writes nothing; validates every pointer as a game heap pointer before reading it.
static void SEH_ReadPrimaryDiag(uintptr_t modBase, size_t modSize, uintptr_t gwObj,
                                PrimaryDiagSnapshot* out) {
    if (gwObj == 0) return;
    // Validador de heap-ptr del juego (mismo criterio que el resto del mod).
    // EN: Game heap pointer validator (same criterion as the rest of the mod).
    auto isHeap = [modBase, modSize](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        if (v >= modBase && v < modBase + modSize) return false;
        return true;
    };
    __try {
        // PlayerInterface = *(GW+0x580). gwObj ya es el objeto GameWorld resuelto.
        // EN: PlayerInterface = *(GW+0x580). gwObj is already the resolved GameWorld object.
        uintptr_t pi = 0;
        if (!Memory::Read(gwObj + 0x580, pi) || !isHeap(pi)) return;
        out->playerIface = pi;
        out->resolved = 1;

        // (1) lektor PI+0x2B0: size@+0x2B8, cap@+0x2BC, data@+0x2C0 → data[0].
        // EN: (1) lektor PI+0x2B0: size@+0x2B8, cap@+0x2BC, data@+0x2C0 → data[0].
        Memory::Read(pi + 0x2B8, out->lektorSize);
        Memory::Read(pi + 0x2BC, out->lektorCap);
        uintptr_t dataPtr = 0;
        if (Memory::Read(pi + 0x2C0, dataPtr) && isHeap(dataPtr) && out->lektorSize > 0) {
            uintptr_t c0 = 0;
            if (Memory::Read(dataPtr, c0) && isHeap(c0)) {
                out->modChar = c0;
                Memory::Read(c0 + 0x10, out->modFaction);     // char+0x10 = faction
                SEH_ReadCharName(c0, out->modName, sizeof(out->modName));
            }
        }

        // (2) PI+0x2A8 = controlledChar (el char que el motor considera "controlado").
        // EN: (2) PI+0x2A8 = controlledChar (the char the engine considers "controlled").
        uintptr_t ctrl = 0;
        if (Memory::Read(pi + 0x2A8, ctrl) && isHeap(ctrl)) {
            out->ctrlChar = ctrl;
            Memory::Read(ctrl + 0x10, out->ctrlFaction);
            SEH_ReadCharName(ctrl, out->ctrlName, sizeof(out->ctrlName));
        }

        // (3) Faction(PI+0x2A0) → memberCount(+0x210) / memberArray(+0x218) → data[count-1].
        // EN: (3) Faction(PI+0x2A0) → memberCount(+0x210) / memberArray(+0x218) → data[count-1].
        uintptr_t fac = 0;
        if (Memory::Read(pi + 0x2A0, fac) && isHeap(fac)) {
            out->faction = fac;
            Memory::Read(fac + 0x210, out->facMemberCnt);
            uintptr_t memArr = 0;
            if (out->facMemberCnt > 0 && out->facMemberCnt < 100000 &&
                Memory::Read(fac + 0x218, memArr) && isHeap(memArr)) {
                uintptr_t last = 0;
                uint32_t  idx  = out->facMemberCnt - 1;  // SetControlledChar usa count-1
                if (Memory::Read(memArr + (uintptr_t)idx * 8, last) && isHeap(last))
                    out->facLastChar = last;
            }
        }

        // Veredictos de coincidencia (POD aritmético, sin objetos C++).
        // EN: Match verdicts (POD arithmetic, no C++ objects).
        if (out->modChar != 0 && out->ctrlChar != 0)
            out->modEqCtrl = (out->modChar == out->ctrlChar) ? 1 : 0;
        if (out->modChar != 0 && out->facLastChar != 0)
            out->modEqFacLast = (out->modChar == out->facLastChar) ? 1 : 0;
        if (out->ctrlChar != 0 && out->facLastChar != 0)
            out->ctrlEqFacLast = (out->ctrlChar == out->facLastChar) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── AITaskSnapshot (Fase 4 — DIAG-AITASK, ¿el char tiene IA/jobs o está "muerto en vida"?) ──
// HIPÓTESIS MÁS FUERTE tras agotar gate(+0x8B9)/seed(+0xD0)/simlist/+0x5BC/char-correcto:
// el char del host (vivo, en simlist, recibe el AI tick 0x5CCD90, entra a la rama viva
// 0x5CD1C0, pasa el gate horario) NO actúa porque su SISTEMA DE TAREAS está nulo o vacío,
// o porque su IA quedó HUÉRFANA del Platoon (AI+0x10 sin registrar).
//
// ⚠ OFFSET CORREGIDO (2026-06-19, RE de bytes confirmado, Steam 1.0.68):
//   El AITaskSytem NO cuelga de char+0x20 (FALSO: char+0x18 es el std::string name SSO de 16
//   bytes, así que char+0x20 cae DENTRO del buffer del nombre → vtbl basura → falso "vtblOk=0").
//   La cadena REAL es DOBLE INDIRECCIÓN:
//       Character + 0x650  ->  AI*           (RTTI .?AVAI@@,          vtable RVA 0x16FA3E8)
//          AI    + 0x20    ->  AITaskSytem*  (RTTI .?AVAITaskSytem@@, vtable RVA 0x16E3F30)
//   CONFIRMADO en attackTarget (0x5CB0A0): `mov rcx,[char+0x650]; mov rcx,[rcx+0x20]` — la cola
//   de combate que el motor consume es *(AI+0x20), NO *(char+0x20). El `mov [rbx+0x20],rax` de
//   AI::create (0x6221AF) escribe AI+0x20 (rbx = AI, cargado por el caller desde char+0x650), no
//   el Character. De ahí el malentendido del DIAG anterior.
//   • Cola de tareas: lektor<Tasker*> INLINE en AITaskSytem+0x2E8 → size@+0x2F0, cap@+0x2F4,
//     data@+0x2F8. ⇒ nº de jobs en cola = *(uint32_t*)(aiTask + 0x2F0).
//
// CAUSAS TRANSVERSALES que este DIAG AMPLIADO sondea lado a lado (host vs NPC sano):
//   (a) AI+0x10 (Platoon registrado): si es 0/basura, el Tasker queda HUÉRFANO → el AI tick no
//       localiza su platoon → NINGÚN Task nuevo se materializa (ni atacar, ni levantarse de cama,
//       ni curarse). Es la causa que ataca TODO Task → amIdle=1 permanente. Predicción: NPC sano
//       AI+0x10!=0; host AI+0x10=0/basura (lo arregla [FIX-PLATOON]).
//   (b) CombatClass = *(CharBody+0x8) (CharBody=char+0x648): si es NULL, falla el ATAQUE concreto
//       (no la causa transversal). Predicción: NPC CombatClass!=0; host CombatClass=0 (lo arregla
//       [FIX-COMBATCLASS]).
//   (c) Tasker individual = *(char+0x448 [AnimationClass*] +0xE8): es el objeto que el AI tick
//       ejecuta vía su vt[+0x10] (=runAction). Si char+0x448 o +0xE8 son NULL, el tick no tiene
//       Task que correr → char vivo pero inerte. Se vuelca para descartar este eslabón.
//
// Comparar host vs NPC discrimina la causa:
//   • host AI==NULL (char+0x650=0)               → el char del host NO tiene subsistema de IA.
//   • host AI+0x10==0 y NPC AI+0x10!=0            → IA huérfana del Platoon = CAUSA TRANSVERSAL (a).
//   • host CombatClass==0 y NPC !=0              → falta la CombatClass = falla el ataque (b).
//   • host aiTask!=NULL y jobs>0 igual que NPC   → la IA/cola está bien; la causa es otra capa.
// SOLO LECTURA, todo bajo SEH. POD puro (sin objetos C++) para vivir dentro de __try (C2712).
// EN: ── AITaskSnapshot (Phase 4 — DIAG-AITASK: does the char have AI/jobs or is it "dead while alive"?) ──
//     STRONGEST HYPOTHESIS after ruling out gate(+0x8B9)/seed(+0xD0)/simlist/+0x5BC/right-char: the host
//     char (alive, in the simlist, receives the AI tick, enters the live branch, passes the hour gate)
//     does NOT act because its TASK SYSTEM is null or empty, or because its AI is ORPHANED from the
//     Platoon (AI+0x10 not registered).
//     OFFSET CORRECTED (2026-06-19, byte RE, Steam 1.0.68): the AITaskSytem does NOT hang from char+0x20
//     (FALSE: char+0x18 is the 16-byte SSO std::string name, so char+0x20 falls INSIDE the name buffer).
//     The REAL chain is a DOUBLE INDIRECTION: Character+0x650 -> AI* (vtable RVA 0x16FA3E8), AI+0x20 ->
//     AITaskSytem* (vtable RVA 0x16E3F30). Confirmed in attackTarget (0x5CB0A0): `mov rcx,[char+0x650];
//     mov rcx,[rcx+0x20]`. Task queue: inline lektor<Tasker*> at AITaskSytem+0x2E8 → size@+0x2F0,
//     cap@+0x2F4, data@+0x2F8.
//     CROSS-CUTTING CAUSES probed side by side (host vs healthy NPC): (a) AI+0x10 (registered Platoon):
//     0/garbage → orphaned Tasker → NO new Task ever materializes (fixed by [FIX-PLATOON]);
//     (b) CombatClass = *(CharBody+0x8): NULL → the ATTACK fails (fixed by [FIX-COMBATCLASS]);
//     (c) individual Tasker = *(char+0x448 [AnimationClass*] +0xE8), run via its vt[+0x10] (runAction):
//     NULL → alive but inert.
//     Host vs NPC comparison tells the cause apart (AI NULL / orphaned AI / missing CombatClass / all OK →
//     another layer). READ ONLY, all under SEH, pure POD (C2712).
struct AITaskSnapshot {
    // Vtables conocidas (RVA, se suman a modBase en el helper para comparar).
    // EN: Known vtables (RVA, added to modBase in the helper to compare).
    static constexpr uintptr_t kAITaskVtableRVA = 0x16E3F30; // AI+0x20 -> AITaskSytem
    static constexpr uintptr_t kAIVtableRVA     = 0x16FA3E8; // char+0x650 -> AI

    // ── Host (char primario del jugador) ──
    // EN: ── Host (player primary char) ── AI (char+0x650), its registered Platoon (AI+0x10), AITaskSytem
    //     (AI+0x20) with its job queue (+0x2F0 size / +0x2F4 cap / +0x2F8 data), CharBody (char+0x648),
    //     CombatClass (*(CharBody+0x8)), AnimationClass (char+0x448), individual Tasker (*(anim+0xE8)) and its
    //     runAction (vtbl+0x10).
    uintptr_t hostChar      = 0;   // el primaryChar que usa el mod
    uintptr_t hostAI        = 0;   // char+0x650 (AI*) — 0 = NULO (sin subsistema de IA)
    int       hostAIVtblOk  = -1;  // 1/0/-1 vtable de hostAI == kAIVtable
    uintptr_t hostPlatoon   = 0;   // AI+0x10 (Platoon* registrado) — 0/basura = IA huérfana = CAUSA (a)
    uintptr_t hostAiTask    = 0;   // AI+0x20 (AITaskSytem*) — 0 = NULO (sin manager de tareas)
    int       hostVtblOk    = -1;  // 1 = vtable de hostAiTask == kAITaskVtable; 0 = distinta; -1 = no leído
    int32_t   hostJobs      = -1;  // AITaskSytem+0x2F0 (size del lektor de Tasker*) — -1 = no leído
    uint32_t  hostJobsCap   = 0;   // AITaskSytem+0x2F4 (capacity) — sanity
    uintptr_t hostJobsData  = 0;   // AITaskSytem+0x2F8 (Tasker** array) — sanity
    uintptr_t hostBody      = 0;   // char+0x648 (CharBody*)
    uintptr_t hostCombat    = 0;   // *(CharBody+0x8) = CombatClass* — 0 = falla el ataque = CAUSA (b)
    uintptr_t hostAnim      = 0;   // char+0x448 (AnimationClass*)
    uintptr_t hostIndivTask = 0;   // *(AnimationClass+0xE8) = Tasker individual que el AI tick ejecuta
    uintptr_t hostIndivRun  = 0;   // *(IndivTask vtbl + 0x10) = runAction (sanity del Tasker individual)

    // ── NPC vecino (primer char de la simlist != host) para comparar ──
    // EN: ── Neighbor NPC (first simlist char != host) for comparison ── same fields as the host.
    uintptr_t npcChar       = 0;   // Character* del NPC vecino encontrado
    char      npcName[32]   = {0}; // nombre del NPC (para identificarlo en el log)
    uintptr_t npcAI         = 0;   // npc+0x650
    int       npcAIVtblOk   = -1;  // 1/0/-1
    uintptr_t npcPlatoon    = 0;   // npc AI+0x10 (debe ser != 0 en un NPC sano = control de la causa a)
    uintptr_t npcAiTask     = 0;   // npc AI+0x20
    int       npcVtblOk     = -1;  // 1/0/-1 como arriba
    int32_t   npcJobs       = -1;  // npc AITaskSytem+0x2F0
    uint32_t  npcJobsCap    = 0;
    uintptr_t npcJobsData   = 0;
    uintptr_t npcBody       = 0;   // npc char+0x648
    uintptr_t npcCombat     = 0;   // *(npc CharBody+0x8) (debe ser != 0 en un NPC sano = control de b)
    uintptr_t npcAnim       = 0;   // npc char+0x448
    uintptr_t npcIndivTask  = 0;   // *(npc AnimationClass+0xE8)
    uintptr_t npcIndivRun   = 0;   // *(npc IndivTask vtbl + 0x10)

    int       hostRead      = 0;   // 1 si se leyó el bloque del host bajo SEH
    int       npcFound      = 0;   // 1 si se encontró un NPC vecino legible
};

// Punteros de salida de UN lado (host o npc) para el DIAG-AITASK ampliado. Agrupa todos los
// campos para no arrastrar una firma con 15 args. Apunta a los miembros del AITaskSnapshot.
// EN: Output pointers of ONE side (host or npc) for the extended DIAG-AITASK. Groups all the fields to
//     avoid a 15-argument signature. Points into the AITaskSnapshot members.
struct AITaskOneSide {
    uintptr_t* ai;          // char+0x650 (AI*)
    int*       aiVtblOk;    // vtable AI == kAIVtable
    uintptr_t* platoon;     // AI+0x10 (Platoon registrado) — 0/basura = IA huérfana = CAUSA (a)
    uintptr_t* aiTask;      // AI+0x20 (AITaskSytem*)
    int*       taskVtblOk;  // vtable AITaskSytem == kAITaskVtable
    int32_t*   jobs;        // AITaskSytem+0x2F0 (size cola)
    uint32_t*  jobsCap;     // +0x2F4
    uintptr_t* jobsData;    // +0x2F8
    uintptr_t* body;        // char+0x648 (CharBody*)
    uintptr_t* combat;      // *(CharBody+0x8) = CombatClass* — 0 = falla el ataque = CAUSA (b)
    uintptr_t* anim;        // char+0x448 (AnimationClass*)
    uintptr_t* indivTask;   // *(AnimationClass+0xE8) = Tasker individual que el AI tick ejecuta
    uintptr_t* indivRun;    // *(IndivTask vtbl + 0x10) = runAction (sanity del Tasker individual)
};

// Lee la cadena de IA/combate de UN Character con los offsets CORREGIDOS y AMPLIADOS:
//   char+0x650 (AI) -> AI+0x10 (Platoon) / AI+0x20 (AITaskSytem) -> +0x2F0 (jobs).
//   char+0x648 (CharBody) -> +0x8 (CombatClass).
//   char+0x448 (AnimationClass) -> +0xE8 (Tasker individual) -> vtbl+0x10 (runAction).
// Rellena los campos del lado (host o npc) vía el AITaskOneSide. Todo bajo SEH del llamador.
// isHeap valida punteros del juego. Devuelve 1 si el AI (char+0x650) resultó legible.
// EN: Reads the AI/combat chain of ONE Character with the CORRECTED and EXTENDED offsets:
//       char+0x650 (AI) -> AI+0x10 (Platoon) / AI+0x20 (AITaskSytem) -> +0x2F0 (jobs).
//       char+0x648 (CharBody) -> +0x8 (CombatClass).
//       char+0x448 (AnimationClass) -> +0xE8 (individual Tasker) -> vtbl+0x10 (runAction).
//     Fills the side fields through AITaskOneSide. Everything under the caller SEH. isHeap validates game
//     pointers. Returns 1 if the AI (char+0x650) was readable.
static int SEH_ReadOneAITask(uintptr_t chr, uintptr_t aiVtableAbs, uintptr_t aiTaskVtableAbs,
                             const AITaskOneSide* s, bool (*isHeap)(uintptr_t)) {
    if (chr <= 0x10000) return 0;
    uintptr_t vtbl = 0;

    // ── (1) AI = char+0x650 (cadena REAL; NO char+0x20). attackTarget hace mov rcx,[char+0x650]. ──
    // EN: ── (1) AI = char+0x650 (REAL chain; NOT char+0x20). attackTarget does mov rcx,[char+0x650]. ──
    uintptr_t ai = 0;
    if (!Memory::Read(chr + 0x650, ai)) return 0;
    *s->ai = ai;
    if (!isHeap(ai)) return 0;                 // sin subsistema de IA → resto N/A
    if (Memory::Read(ai, vtbl)) *s->aiVtblOk = (vtbl == aiVtableAbs) ? 1 : 0;

    // ── (2a) Platoon registrado = AI+0x10 (lo que setActivePlatoon→0x506CC0 escribe). ──
    //   0/basura = el Tasker del platoon queda HUÉRFANO → el AI tick no consume Tasks = CAUSA (a).
    // EN: ── (2a) Registered Platoon = AI+0x10 (what setActivePlatoon→0x506CC0 writes). 0/garbage = the
    //     platoon Tasker is ORPHANED → the AI tick does not consume Tasks = CAUSE (a). ──
    Memory::Read(ai + 0x10, *s->platoon);

    // ── (2b) AITaskSytem = AI+0x20 (la COLA de combate que el motor consume). ──
    // EN: ── (2b) AITaskSytem = AI+0x20 (the combat QUEUE the engine consumes). ──
    uintptr_t aiTask = 0;
    if (Memory::Read(ai + 0x20, aiTask)) {
        *s->aiTask = aiTask;
        if (isHeap(aiTask)) {
            if (Memory::Read(aiTask, vtbl)) *s->taskVtblOk = (vtbl == aiTaskVtableAbs) ? 1 : 0;
            // Cola de jobs: lektor<Tasker*> INLINE en AITaskSytem+0x2E8 → size@+0x2F0/cap@+0x2F4/data@+0x2F8.
            // EN: Job queue: inline lektor<Tasker*> at AITaskSytem+0x2E8 → size@+0x2F0/cap@+0x2F4/data@+0x2F8.
            uint32_t size = 0, cap = 0;
            if (Memory::Read(aiTask + 0x2F0, size))
                *s->jobs = (size < 100000u) ? (int32_t)size : (int32_t)0x7FFFFFFF;  // cota anti-basura
            if (Memory::Read(aiTask + 0x2F4, cap)) *s->jobsCap = cap;
            Memory::Read(aiTask + 0x2F8, *s->jobsData);
        }
    }

    // ── (3) CombatClass = *(char+0x648 [CharBody] +0x8) — 0 = falla el ataque concreto = CAUSA (b). ──
    // EN: ── (3) CombatClass = *(char+0x648 [CharBody] +0x8) — 0 = the concrete attack fails = CAUSE (b). ──
    uintptr_t body = 0;
    if (Memory::Read(chr + 0x648, body)) {
        *s->body = body;
        if (isHeap(body)) Memory::Read(body + 0x8, *s->combat);
    }

    // ── (4) Tasker individual = *(char+0x448 [AnimationClass*] +0xE8). El AI tick (0x5CCD90) lo
    //        ejecuta vía su vt[+0x10] (=runAction). Si anim o +0xE8 son NULL, el tick no corre Task. ──
    // EN: ── (4) Individual Tasker = *(char+0x448 [AnimationClass*] +0xE8). The AI tick (0x5CCD90) runs it
    //     through its vt[+0x10] (=runAction). If anim or +0xE8 are NULL the tick runs no Task. ──
    uintptr_t anim = 0;
    if (Memory::Read(chr + 0x448, anim)) {
        *s->anim = anim;
        if (isHeap(anim)) {
            uintptr_t indiv = 0;
            if (Memory::Read(anim + 0xE8, indiv)) {
                *s->indivTask = indiv;
                uintptr_t ivtbl = 0;
                if (isHeap(indiv) && Memory::Read(indiv, ivtbl))
                    Memory::Read(ivtbl + 0x10, *s->indivRun);  // runAction del Tasker individual
            }
        }
    }
    return 1;
}

// ── SEH_ReadAITaskDiag (DIAG-AITASK) — host vs NPC vecino. SOLO LECTURA ──
// Vuelca, con los OFFSETS CORREGIDOS, la cadena AI(char+0x650)→Platoon(+0x10)/AITaskSytem(+0x20)
// + CombatClass(*(char+0x648+0x8)) + Tasker individual(*(char+0x448+0xE8)) del host y de un NPC
// vecino de la simlist (control sano). El NPC vecino = primer Character de la lista de simulación
// (GW+0x768/+0x788) que NO sea el host.
// EN: ── SEH_ReadAITaskDiag (DIAG-AITASK) — host vs neighbor NPC. READ ONLY ── Dumps, with the CORRECTED
//     OFFSETS, the chain AI(char+0x650)→Platoon(+0x10)/AITaskSytem(+0x20) + CombatClass + individual
//     Tasker of the host and of a neighbor simlist NPC (healthy control) = first Character of the
//     simulation list (GW+0x768/+0x788) that is NOT the host.
static void SEH_ReadAITaskDiag(uintptr_t modBase, uintptr_t gwObj, void* primaryChar,
                               AITaskSnapshot* out) {
    const uintptr_t aiVtblAbs     = modBase + AITaskSnapshot::kAIVtableRVA;     // char+0x650 -> AI
    const uintptr_t aiTaskVtblAbs = modBase + AITaskSnapshot::kAITaskVtableRVA; // AI+0x20    -> AITaskSytem
    // Validador de heap-ptr del juego (mismo criterio que el resto del mod).
    // EN: Game heap pointer validator (same criterion as the rest of the mod).
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;   // punteros del juego están alineados a 8
        return true;
    };
    // Punteros de salida de cada lado (apuntan a los miembros del snapshot).
    // EN: Output pointers of each side (they point into the snapshot members).
    AITaskOneSide hostSide{ &out->hostAI, &out->hostAIVtblOk, &out->hostPlatoon,
                            &out->hostAiTask, &out->hostVtblOk, &out->hostJobs,
                            &out->hostJobsCap, &out->hostJobsData, &out->hostBody,
                            &out->hostCombat, &out->hostAnim, &out->hostIndivTask, &out->hostIndivRun };
    AITaskOneSide npcSide{  &out->npcAI, &out->npcAIVtblOk, &out->npcPlatoon,
                            &out->npcAiTask, &out->npcVtblOk, &out->npcJobs,
                            &out->npcJobsCap, &out->npcJobsData, &out->npcBody,
                            &out->npcCombat, &out->npcAnim, &out->npcIndivTask, &out->npcIndivRun };
    __try {
        // ── HOST ──
        // EN: ── HOST ──
        uintptr_t host = reinterpret_cast<uintptr_t>(primaryChar);
        out->hostChar = host;
        if (host > 0x10000) {
            out->hostRead = SEH_ReadOneAITask(host, aiVtblAbs, aiTaskVtblAbs, &hostSide, isHeap);
        }

        // ── NPC VECINO: recorrer la simlist igual que SEH_WalkSimList y agarrar el
        //    primer Character != host. Así comparamos el host con un char que el motor
        //    sí simula con normalidad (su IA fue creada por el flujo estándar / AI::create). ──
        // EN: ── NEIGHBOR NPC: walk the simlist like SEH_WalkSimList and take the first Character != host, so the
        //     host is compared with a char the engine simulates normally (its AI came from the standard flow). ──
        if (gwObj != 0) {
            uint64_t count = 0;
            if (Memory::Read(gwObj + 0x770, count) && count > 0) {
                uintptr_t arrayPtr = 0, headIdx = 0;
                if (Memory::Read(gwObj + 0x788, arrayPtr) && arrayPtr > 0x10000 &&
                    Memory::Read(gwObj + 0x768, headIdx)) {
                    uintptr_t node = 0;
                    if (Memory::Read(arrayPtr + headIdx * 8, node)) {
                        const uint32_t kMaxIter = 20000;
                        uint32_t walked = 0;
                        while (node > 0x10000 && walked < kMaxIter && out->npcFound == 0) {
                            walked++;
                            uintptr_t chr = 0;
                            if (Memory::Read(node + 0x10, chr) && isHeap(chr) && chr != host) {
                                // candidato NPC: leer su cadena AI/combate (control sano).
                                out->npcChar = chr;
                                SEH_ReadCharName(chr, out->npcName, sizeof(out->npcName));
                                SEH_ReadOneAITask(chr, aiVtblAbs, aiTaskVtblAbs, &npcSide, isHeap);
                                out->npcFound = 1;
                                break;
                            }
                            uintptr_t next = 0;
                            if (!Memory::Read(node + 0x00, next)) break;
                            if (next == node) break;
                            node = next;
                        }
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── CombatStructSnapshot (Fase 4 — DIAG-COMBATSTRUCT) ────────────────────────────────
// PRUEBA DEFINITIVA del cluster de subsistemas IA/combate del Character. CORRIGE el error
// del [DIAG-AITASK] previo, que leía el AITaskSytem en char+0x20 (OFFSET FALSO: char+0x18 es
// el std::string name SSO de 16 bytes, así que char+0x20 cae DENTRO del buffer del nombre →
// daba floats basura). Doble RE de bytes (game-reverse-engineer, Steam 1.0.68, 2026-06-18):
//
//   El AITaskSytem NO cuelga del Character directamente. La cadena REAL es doble indirección:
//       Character + 0x650  ->  AI*            (RTTI .?AVAI@@,          vtable RVA 0x16FA3E8)
//          AI    + 0x20    ->  AITaskSytem*   (RTTI .?AVAITaskSytem@@, vtable RVA 0x16E3F30)
//   El `mov [rbx+0x20],rax` de AI::create (0x6221AF) escribe AI+0x20, NO char+0x20: el caller
//   (0x62B1C2) carga rcx = [char+0x650] (el objeto AI), no el Character. De ahí el malentendido.
//
//   Cluster completo (creado en Character::initAISubsystems 0x62AF50, alcanzado vía
//   Character::activate 0x62B210 por método VIRTUAL durante el spawn natural):
//       char+0x640 -> CharMovement* (.?AVCharMovement@@, vtable 0x16FCC88) — locomoción/animación
//       char+0x648 -> CharBody*     (.?AVCharBody@@,     vtable 0x16F8A68)
//       char+0x650 -> AI*           (.?AVAI@@,           vtable 0x16FA3E8) — y AI+0x20=AITaskSytem*
//
// HIPÓTESIS QUE DISCRIMINA (la del usuario, refinada con offsets correctos): el char del host,
// reclamado por el spawn de plantilla del mod (FactoryCreate/SpawnWithModTemplate), pudo SALTARSE
// la cadena de activación virtual → quedó con char+0x650 (AI*) o AI+0x20 (AITaskSytem*) NULOS.
//   • CharMovement (char+0x640) presente  →  el host CAMINA (locomoción no necesita AITaskSytem).
//   • AI / AITaskSytem ausentes           →  el host NO piensa ni combate (sin manager de tareas
//                                            que procese órdenes de ataque ni IA hostil) = CAUSA.
// Comparamos host vs NPC vecino (char de la simlist que el motor sí simula con normalidad):
//   host AI/AITaskSytem == NULL  &&  NPC != NULL   →  CAUSA CONFIRMADA (spawn del mod se saltó init).
//   host y NPC ambos válidos                       →  el cluster está OK; causa en OTRA capa (gate
//                                                     GW+0x8B9 ya documentado / encolado de jobs).
//
// NOTA sobre char+0x178 (que la hipótesis previa llamaba "Combat stats base"): RE de bytes lo
// REFUTA — es un std::unordered_map<string,T> INLINE (no una combat-struct), y ApplyDamage
// (0x7A33A0) NI SIQUIERA lo accede. No se vuelca aquí porque no es la estructura de combate.
// SOLO LECTURA, todo bajo SEH. POD puro (sin objetos C++) para vivir dentro de __try (C2712).
// EN: ── CombatStructSnapshot (Phase 4 — DIAG-COMBATSTRUCT) ── DEFINITIVE TEST of the Character AI/combat
//     subsystem cluster. FIXES the previous [DIAG-AITASK] error that read the AITaskSytem at char+0x20
//     (FALSE offset, inside the SSO name buffer). Real chain (double byte RE, 2026-06-18):
//     Character+0x650 -> AI* (vtable RVA 0x16FA3E8), AI+0x20 -> AITaskSytem* (vtable RVA 0x16E3F30).
//     Full cluster (created in Character::initAISubsystems 0x62AF50, reached through Character::activate
//     0x62B210 during natural spawn): char+0x640 -> CharMovement* (vtable 0x16FCC88, locomotion),
//     char+0x648 -> CharBody* (vtable 0x16F8A68), char+0x650 -> AI* (vtable 0x16FA3E8).
//     HYPOTHESIS: the host char, claimed through the mod template spawn, may have SKIPPED the virtual
//     activation chain → AI or AITaskSytem NULL. CharMovement present → the host WALKS; AI/AITaskSytem
//     missing → it does NOT think nor fight. Host NULL && NPC != NULL → CAUSE CONFIRMED; both valid →
//     the cause is in ANOTHER layer.
//     NOTE on char+0x178 (formerly called "combat stats base"): byte RE REFUTES it — it is an inline
//     std::unordered_map<string,T>, and ApplyDamage (0x7A33A0) does not even touch it.
//     READ ONLY, all under SEH, pure POD (C2712).
struct CombatStructSnapshot {
    // Vtables conocidas (RVA, se suman a modBase en el helper para comparar).
    // EN: Known vtables (RVA, added to modBase in the helper to compare): AI, AITaskSytem, CharMovement and,
    //     for [DIAG-ATTACK], CombatClass, CharBody, base Tasker, Task_MeleeAttack (automatic attack) and
    //     Task_FocusedMeleeAttack (player-ordered attack, RE 2026-07-14: recognizing it avoids a false
    //     "order lost / host busy" negative when the attack does work).
    static constexpr uintptr_t kAIVtableRVA           = 0x16FA3E8; // char+0x650 -> AI
    static constexpr uintptr_t kAITaskVtableRVA       = 0x16E3F30; // AI+0x20    -> AITaskSytem
    static constexpr uintptr_t kCharMovementVtableRVA = 0x16FCC88; // char+0x640 -> CharMovement
    // ── [DIAG-ATTACK] vtables de la capa de combate (RE Steam 1.0.68, audit-12) ──
    static constexpr uintptr_t kCombatClassVtableRVA  = 0x16F67B8; // *(CharBody+0x8) -> CombatClass (vtbl derivada)
    static constexpr uintptr_t kCharBodyVtableRVA     = 0x16F8A68; // char+0x648 -> CharBody
    static constexpr uintptr_t kTaskerVtableRVA       = 0x16BDC68; // Task activo base (Tasker)
    static constexpr uintptr_t kTaskMeleeVtableRVA    = 0x16BE448; // Task activo = Task_MeleeAttack (ataque AUTOMÁTICO del CombatClass)
    // ── Ataque ORDENADO por el jugador (clic manual): el motor usa una tarea DISTINTA (RTTI en vivo,
    //    RE 2026-07-14). El host SÍ fija currentTarget y ejecuta esta tarea contra enemigos reales.
    //    Reconocerla evita el falso negativo "orden perdida / host ocupado" cuando el ataque SÍ funciona.
    static constexpr uintptr_t kTaskFocusedMeleeVtableRVA = 0x16BF9E8; // Task activo = Task_FocusedMeleeAttack (ataque ORDENADO)

    // ── Host (char primario del jugador) ──
    // EN: ── Host (player primary char) ── movement/AI/AITaskSytem with vtable checks and job count, plus the
    //     [DIAG-ATTACK] combat layer: AnimationClass (char+0x448), CharBody (char+0x648), CombatClass
    //     (*(CharBody+0x8), 0 = CAUSE A), currentTarget (CombatClass+0x290), active Tasker (*(CharBody+0x68))
    //     with its vtable, and amIdle (CharBody+0x70).
    uintptr_t hostChar       = 0;   // primaryChar del mod
    uintptr_t hostMovement   = 0;   // char+0x640 (CharMovement*) — 0 = NULO
    int       hostMoveVtblOk = -1;  // 1/0/-1 vtable de hostMovement == kCharMovement
    uintptr_t hostAI         = 0;   // char+0x650 (AI*) — 0 = NULO (sin subsistema de IA)
    int       hostAIVtblOk   = -1;  // 1/0/-1 vtable de hostAI == kAI
    uintptr_t hostAiTask     = 0;   // AI+0x20 (AITaskSytem*) — 0 = NULO (sin manager de tareas)
    int       hostTaskVtblOk = -1;  // 1/0/-1 vtable de hostAiTask == kAITask
    int32_t   hostJobs       = -1;  // AITaskSytem+0x2F0 (size del lektor de Tasker*) — -1 = no leído
    int       hostRead       = 0;   // 1 si se leyó el bloque del host bajo SEH
    // ── [DIAG-ATTACK] capa de combate del host ──
    uintptr_t hostAnim         = 0; // char+0x448 (AnimationClass*) — 0 = anómalo (juego crashearía)
    uintptr_t hostBody         = 0; // char+0x648 (CharBody*) — 0 = giveBirth no corrió (variante extrema)
    int       hostBodyVtblOk   = -1;// 1/0/-1 vtable de hostBody == kCharBody
    uintptr_t hostCombat       = 0; // *(CharBody+0x8) = CombatClass* — 0 = CAUSA A confirmada (LA SONDA)
    int       hostCombatVtblOk = -1;// 1/0/-1 vtable de hostCombat == kCombatClass
    uintptr_t hostTarget       = 0; // CombatClass+0x290 (currentTarget) — 0 salvo orden de ataque activa
    uintptr_t hostActiveTask   = 0; // *(CharBody+0x68) = CharBody.currentAction (Tasker activo) — 0 = sin tarea
    uintptr_t hostActiveTaskVtbl = 0; // vtable del Tasker activo (comparar vs Tasker/Task_MeleeAttack)
    int       hostAmIdle       = -1;// CharBody+0x70 (bool) — 1=ocioso (gate rechazó), 0=ejecutando Task, -1=no leído

    // ── NPC vecino (primer char de la simlist != host) para comparar ──
    // EN: ── Neighbor NPC (first simlist char != host) for comparison ── same fields (healthy control).
    uintptr_t npcChar        = 0;
    char      npcName[32]    = {0};
    uintptr_t npcMovement    = 0;
    int       npcMoveVtblOk  = -1;
    uintptr_t npcAI          = 0;
    int       npcAIVtblOk    = -1;
    uintptr_t npcAiTask      = 0;
    int       npcTaskVtblOk  = -1;
    int32_t   npcJobs        = -1;
    int       npcFound       = 0;   // 1 si se encontró un NPC vecino legible
    // ── [DIAG-ATTACK] capa de combate del NPC (control sano para comparar) ──
    uintptr_t npcAnim         = 0;
    uintptr_t npcBody         = 0;
    int       npcBodyVtblOk   = -1;
    uintptr_t npcCombat       = 0;  // *(CharBody+0x8) del NPC — debe ser != 0 (control)
    int       npcCombatVtblOk = -1;
    uintptr_t npcTarget       = 0;
    uintptr_t npcActiveTask   = 0;
    uintptr_t npcActiveTaskVtbl = 0;
    int       npcAmIdle       = -1; // CharBody+0x70 del NPC (control: un NPC en combate debe tener amIdle=0)
};

// Salida de la capa de combate del [DIAG-ATTACK] (POD; se rellena dentro del SEH del llamador).
// Agrupa los 4 datos decisivos que el RE (audit-12) identificó como la sonda de la CausaA:
// AnimationClass, CharBody, CombatClass (= *(CharBody+0x8) — LA SONDA), currentTarget y Task activo.
// EN: Output of the [DIAG-ATTACK] combat layer (POD; filled inside the caller SEH). Groups the decisive
//     data the RE (audit-12) identified as the CauseA probe: AnimationClass, CharBody, CombatClass
//     (= *(CharBody+0x8) — THE PROBE), currentTarget and active Task.
struct CombatLayerOut {
    uintptr_t* anim;          // char+0x448 (AnimationClass*)
    uintptr_t* body;          // char+0x648 (CharBody*)
    int*       bodyOk;        // vtable CharBody == kCharBody
    uintptr_t* combat;        // *(CharBody+0x8) = CombatClass* — 0 = CAUSA A
    int*       combatOk;      // vtable CombatClass == kCombatClass
    uintptr_t* target;        // CombatClass+0x290 (currentTarget)
    uintptr_t* activeTask;    // ✅ CORREGIDO: *(CharBody+0x68) = CharBody.currentAction (Tasker activo)
    uintptr_t* activeTaskVtbl;// vtable del Tasker activo (comparar vs Tasker/Task_MeleeAttack)
    int*       amIdle;        // CharBody+0x70 (bool) — 1 = char ocioso (sin Task) ; 0 = ejecutando Task
};

// Lee el cluster IA/combate (CharMovement/AI/AITaskSytem) de UN Character, validando vtables.
// Rellena los campos *Movement/*AI/*AiTask/*Jobs del out (host o npc según los punteros pasados).
// Si 'cl' != nullptr, también vuelca la CAPA DE COMBATE ([DIAG-ATTACK]): AnimationClass, CharBody,
// CombatClass (=*(CharBody+0x8) — la sonda decisiva de la CausaA), currentTarget y Task activo.
// Todo bajo SEH del llamador. isHeap valida punteros del juego. Devuelve 1 si el char es legible.
// EN: Reads the AI/combat cluster (CharMovement/AI/AITaskSytem) of ONE Character, validating vtables.
//     Fills the xxxMovement/xxxAI/xxxAiTask/xxxJobs out fields (host or npc depending on the pointers). If 'cl'
//     != nullptr it also dumps the COMBAT LAYER ([DIAG-ATTACK]). Everything under the caller SEH. isHeap
//     validates game pointers. Returns 1 if the char is readable.
static int SEH_ReadOneCombatStruct(uintptr_t chr,
                                    uintptr_t aiVtblAbs, uintptr_t taskVtblAbs, uintptr_t moveVtblAbs,
                                    uintptr_t* outMove, int* outMoveOk,
                                    uintptr_t* outAI,   int* outAIOk,
                                    uintptr_t* outTask, int* outTaskOk, int32_t* outJobs,
                                    bool (*isHeap)(uintptr_t),
                                    const CombatLayerOut* cl,
                                    uintptr_t bodyVtblAbs, uintptr_t combatVtblAbs) {
    if (chr <= 0x10000) return 0;
    uintptr_t vtbl = 0;

    // char+0x640 = CharMovement* (locomoción/animación — lo que permite CAMINAR).
    // EN: char+0x640 = CharMovement* (locomotion/animation — what allows WALKING).
    uintptr_t move = 0;
    if (Memory::Read(chr + 0x640, move)) {
        *outMove = move;
        if (isHeap(move) && Memory::Read(move, vtbl))
            *outMoveOk = (vtbl == moveVtblAbs) ? 1 : 0;
    }

    // char+0x650 = AI* (subsistema de IA/combate — lo que falta si el host no piensa).
    // EN: char+0x650 = AI* (AI/combat subsystem — what is missing if the host does not think).
    uintptr_t ai = 0;
    if (Memory::Read(chr + 0x650, ai)) {
        *outAI = ai;
        if (isHeap(ai) && Memory::Read(ai, vtbl)) {
            *outAIOk = (vtbl == aiVtblAbs) ? 1 : 0;

            // AI+0x20 = AITaskSytem* (manager de tareas; su cola procesa órdenes de ataque).
            // EN: AI+0x20 = AITaskSytem* (task manager; its queue processes attack orders).
            uintptr_t task = 0;
            if (Memory::Read(ai + 0x20, task)) {
                *outTask = task;
                if (isHeap(task) && Memory::Read(task, vtbl)) {
                    *outTaskOk = (vtbl == taskVtblAbs) ? 1 : 0;
                    // Cola de jobs: lektor<Tasker*> INLINE en AITaskSytem+0x2E8, size@+0x2F0.
                    // EN: Job queue: inline lektor<Tasker*> at AITaskSytem+0x2E8, size@+0x2F0.
                    uint32_t size = 0;
                    if (Memory::Read(task + 0x2F0, size))
                        *outJobs = (size < 100000u) ? (int32_t)size : (int32_t)0x7FFFFFFF;
                }
            }
        }
    }

    // ── [DIAG-ATTACK] CAPA DE COMBATE (opcional) ──────────────────────────────────────
    // Razón: el char puede tener AI/AITaskSytem completos y AUN ASÍ no atacar si le falta
    // la CombatClass (= *(CharBody+0x8)). Esa es la CAUSA A confirmada por RE (audit-12):
    // el spawn del mod se salta giveBirth→CharBody::create, dejando CharBody+0x8 = NULL.
    // EN: ── [DIAG-ATTACK] COMBAT LAYER (optional) ── The char may have a full AI/AITaskSytem and STILL not
    //     attack if it lacks the CombatClass (= *(CharBody+0x8)). That is CAUSE A confirmed by RE (audit-12):
    //     the mod spawn skips giveBirth→CharBody::create, leaving CharBody+0x8 = NULL.
    if (cl) {
        // (1) AnimationClass char+0x448 — debe ser != 0 (si fuese 0, el juego crashearía).
        // EN: (1) AnimationClass char+0x448 — must be != 0 (if 0, the game would crash).
        uintptr_t anim = 0;
        if (Memory::Read(chr + 0x448, anim)) *cl->anim = anim;

        // (2) CharBody char+0x648 → CombatClass = *(CharBody+0x8) — LA SONDA DECISIVA.
        // EN: (2) CharBody char+0x648 → CombatClass = *(CharBody+0x8) — THE DECISIVE PROBE.
        uintptr_t body = 0;
        if (Memory::Read(chr + 0x648, body)) {
            *cl->body = body;
            if (isHeap(body) && Memory::Read(body, vtbl)) {
                *cl->bodyOk = (vtbl == bodyVtblAbs) ? 1 : 0;
                uintptr_t combat = 0;
                if (Memory::Read(body + 0x8, combat)) {
                    *cl->combat = combat;                       // 0 = CAUSA A confirmada
                    if (isHeap(combat) && Memory::Read(combat, vtbl)) {
                        *cl->combatOk = (vtbl == combatVtblAbs) ? 1 : 0;
                        // (3) currentTarget = CombatClass+0x290 (NULL salvo orden de ataque activa).
                        // EN: (3) currentTarget = CombatClass+0x290 (NULL unless an attack order is active).
                        Memory::Read(combat + 0x290, *cl->target);
                    }
                }
            }
        }

        // (4) Task activo = *(CharBody+0x68) = CharBody.currentAction (Tasker*).  ✅ CORREGIDO 2026-06-19
        //   ⚠ El código previo leía *(char+0x448 [AnimationClass] +0xE8): eso NO es el Task activo
        //   (char+0x448 es AppearanceHuman/AnimationClass; +0xE8 daba un falso negativo). El Task que
        //   el char está ejecutando vive en CharBody.currentAction (+0x68, CONFIRMADO KenshiLib
        //   CharBody.h:82). Y CharBody.amIdle (+0x70, bool) dice si el char está ocioso (sin Task).
        //   Estos dos discriminan en runtime: amIdle=1 → no hay orden ejecutándose (gate la rechazó);
        //   activeTaskVtbl == Task_MeleeAttack(0x16BE448) → el ataque SÍ se encoló (gate lo permitió).
        // EN: (4) Active Task = *(CharBody+0x68) = CharBody.currentAction (Tasker*). CORRECTED 2026-06-19:
        //     the old code read *(char+0x448 [AnimationClass] +0xE8), which is NOT the active Task (false
        //     negative). The running Task lives in CharBody.currentAction (+0x68, confirmed by KenshiLib
        //     CharBody.h:82) and CharBody.amIdle (+0x70, bool) says whether the char is idle. amIdle=1 → no order
        //     running (the gate rejected it); activeTaskVtbl == Task_MeleeAttack (0x16BE448) → the attack WAS
        //     queued.
        if (isHeap(body)) {
            uintptr_t activeTask = 0;
            if (Memory::Read(body + 0x68, activeTask)) {          // CharBody.currentAction
                *cl->activeTask = activeTask;
                if (isHeap(activeTask) && Memory::Read(activeTask, vtbl))
                    *cl->activeTaskVtbl = vtbl; // comparar vs Tasker 0x16BDC68 / Task_MeleeAttack 0x16BE448
            }
            uint8_t idle = 0;
            if (Memory::Read(body + 0x70, idle)) *cl->amIdle = idle ? 1 : 0;  // CharBody.amIdle
        }
    }
    return 1;
}

// ── SEH_ReadCombatStructDiag (DIAG-COMBATSTRUCT) — host vs NPC vecino. SOLO LECTURA ──
// Vuelca el cluster CharMovement/AI/AITaskSytem del host y de un NPC vecino de la simlist.
// El NPC vecino = primer Character de la lista de simulación (GW+0x768/+0x788) que NO sea el host.
// EN: ── SEH_ReadCombatStructDiag (DIAG-COMBATSTRUCT) — host vs neighbor NPC. READ ONLY ── Dumps the
//     CharMovement/AI/AITaskSytem cluster of the host and of a neighbor simlist NPC (first Character of
//     the simulation list GW+0x768/+0x788 that is NOT the host).
static void SEH_ReadCombatStructDiag(uintptr_t modBase, uintptr_t gwObj, void* primaryChar,
                                     CombatStructSnapshot* out) {
    const uintptr_t aiVtblAbs     = modBase + CombatStructSnapshot::kAIVtableRVA;
    const uintptr_t taskVtblAbs   = modBase + CombatStructSnapshot::kAITaskVtableRVA;
    const uintptr_t moveVtblAbs   = modBase + CombatStructSnapshot::kCharMovementVtableRVA;
    const uintptr_t bodyVtblAbs   = modBase + CombatStructSnapshot::kCharBodyVtableRVA;
    const uintptr_t combatVtblAbs = modBase + CombatStructSnapshot::kCombatClassVtableRVA;
    // Validador de heap-ptr del juego (mismo criterio que el resto del mod: alineado a 8).
    // EN: Game heap pointer validator (same criterion as the rest of the mod: 8-byte aligned).
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        return true;
    };
    // Punteros de salida de la capa de combate del host y del NPC (para el [DIAG-ATTACK]).
    // EN: Output pointers of the host and NPC combat layer (for [DIAG-ATTACK]).
    CombatLayerOut hostCL{ &out->hostAnim, &out->hostBody, &out->hostBodyVtblOk,
                           &out->hostCombat, &out->hostCombatVtblOk, &out->hostTarget,
                           &out->hostActiveTask, &out->hostActiveTaskVtbl, &out->hostAmIdle };
    CombatLayerOut npcCL{  &out->npcAnim, &out->npcBody, &out->npcBodyVtblOk,
                           &out->npcCombat, &out->npcCombatVtblOk, &out->npcTarget,
                           &out->npcActiveTask, &out->npcActiveTaskVtbl, &out->npcAmIdle };
    __try {
        // ── HOST ──
        // EN: ── HOST ──
        uintptr_t host = reinterpret_cast<uintptr_t>(primaryChar);
        out->hostChar = host;
        if (host > 0x10000) {
            out->hostRead = SEH_ReadOneCombatStruct(host, aiVtblAbs, taskVtblAbs, moveVtblAbs,
                                                     &out->hostMovement, &out->hostMoveVtblOk,
                                                     &out->hostAI,       &out->hostAIVtblOk,
                                                     &out->hostAiTask,   &out->hostTaskVtblOk,
                                                     &out->hostJobs, isHeap,
                                                     &hostCL, bodyVtblAbs, combatVtblAbs);
        }

        // ── NPC VECINO: recorrer la simlist igual que SEH_ReadAITaskDiag y agarrar el primer
        //    Character != host (su cluster fue creado por el flujo de activación estándar). ──
        // EN: ── NEIGHBOR NPC: walk the simlist like SEH_ReadAITaskDiag and take the first Character != host
        //     (its cluster was created by the standard activation flow). ──
        if (gwObj != 0) {
            uint64_t count = 0;
            if (Memory::Read(gwObj + 0x770, count) && count > 0) {
                uintptr_t arrayPtr = 0, headIdx = 0;
                if (Memory::Read(gwObj + 0x788, arrayPtr) && arrayPtr > 0x10000 &&
                    Memory::Read(gwObj + 0x768, headIdx)) {
                    uintptr_t node = 0;
                    if (Memory::Read(arrayPtr + headIdx * 8, node)) {
                        const uint32_t kMaxIter = 20000;
                        uint32_t walked = 0;
                        while (node > 0x10000 && walked < kMaxIter && out->npcFound == 0) {
                            walked++;
                            uintptr_t chr = 0;
                            if (Memory::Read(node + 0x10, chr) && isHeap(chr) && chr != host) {
                                out->npcChar = chr;
                                SEH_ReadCharName(chr, out->npcName, sizeof(out->npcName));
                                SEH_ReadOneCombatStruct(chr, aiVtblAbs, taskVtblAbs, moveVtblAbs,
                                                        &out->npcMovement, &out->npcMoveVtblOk,
                                                        &out->npcAI,       &out->npcAIVtblOk,
                                                        &out->npcAiTask,   &out->npcTaskVtblOk,
                                                        &out->npcJobs, isHeap,
                                                        &npcCL, bodyVtblAbs, combatVtblAbs);
                                out->npcFound = 1;
                                break;
                            }
                            uintptr_t next = 0;
                            if (!Memory::Read(node + 0x00, next)) break;
                            if (next == node) break;
                            node = next;
                        }
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Lee el GATE real de pausa GW+0x8B9 y el gameSpeed GW+0x700 en una pasada SEH.
// Devuelve true si leyó el gate; *gate = valor del flag, *speed = gameSpeed.
// El gate es lo que mainLoop (0x788FF5) consulta para decidir si corre updateCharacters.
// EN: Reads the REAL pause gate GW+0x8B9 and gameSpeed GW+0x700 in one SEH pass. Returns true if the gate
//     was read; *gate = flag value, *speed = gameSpeed. The gate is what mainLoop (0x788FF5) checks to
//     decide whether updateCharacters runs.
static bool SEH_ReadPauseGate(uintptr_t gwObj, uint8_t* gate, float* speed) {
    bool read = false;
    __try {
        read = Memory::Read(gwObj + 0x8B9, *gate);
        Memory::Read(gwObj + 0x700, *speed);
    } __except (EXCEPTION_EXECUTE_HANDLER) { read = false; }
    return read;
}

// Restaura gameSpeed GW+0x700 a 1.0 (SEH-aislado). Necesario ANTES de despausar:
// el setter oficial 0x787D40 hace GW+0x8B9 = argBool OR (gameSpeed==0.0f), así que con
// gameSpeed clavado en 0.0 el flag se re-pega a 1 aunque pasemos false. Con speed=1.0 el
// OR no dispara y la despausa pasa limpia.
// EN: Restores gameSpeed GW+0x700 to 1.0 (SEH-isolated). Needed BEFORE unpausing: the official setter
//     0x787D40 does GW+0x8B9 = argBool OR (gameSpeed==0.0f), so with gameSpeed stuck at 0.0 the flag
//     sticks back to 1 even when passing false. With speed=1.0 the OR does not fire and unpausing works.
static bool SEH_RestoreGameSpeed(uintptr_t gwObj) {
    bool wrote = false;
    __try {
        float one = 1.0f;
        wrote = Memory::Write(gwObj + 0x700, one);
    } __except (EXCEPTION_EXECUTE_HANDLER) { wrote = false; }
    return wrote;
}

// ── HostilityDiagSnapshot (Fase 4 — DIAG-HOSTILITY) ──────────────────────────────────
// PRUEBA DEFINITIVA de por qué el combate co-op está congelado de forma SIMÉTRICA (el host
// no ataca a bandidos y los bandidos no le atacan). RE de bytes (game-reverse-engineer,
// Steam 1.0.68, 2026-06-18, doble verificación independiente iced-x86):
//
//   La hostilidad en Kenshi NO la decide el "fundamental type" (OT_CIVILIAN=4) — eso REFUTA
//   la afirmación del audit-05. La decide una RELACIÓN FLOAT EXPLÍCITA entre Faction objects.
//   El núcleo es isEnemy (RVA 0x6B26D0): bool isEnemy(FactionRelations* /*Faction+0x78*/,
//   Faction* other) => devuelve (relacion <= -30.0). 24 callers de combate/IA la usan ANTES
//   de hacer aggro; NINGUNO comprueba el type antes. Umbrales: <= -30.0 ENEMIGO, >= +50.0
//   ALIADO, intermedio NEUTRAL.
//
//   Una Faction recién clonada (las "Player N" que ModGen copia de "Player 1" en el FCS) nace
//   con su mapa de relaciones VACÍO: FactionRelations es un std::unordered_map<Faction*,float>
//   MSVC embebido en Faction+0x78, cuyo map interno empieza en (Faction+0x78)+0x20. Si el clon
//   no heredó las entradas de relación que la "Player 1" original tiene con las facciones
//   hostiles vanilla, TODAS las relaciones resuelven a 0.0 (neutro) => -30 < 0 < +50 =>
//   isEnemy=false e isAlly=false con TODO el mundo. Resultado: nadie computa hostilidad mutua
//   => combate simétrico congelado. ESTA es la causa raíz candidata.
//
// NO se puede llamar a isEnemy(0x6B26D0) desde el DIAG: su lookup interno (getRelationEntry,
// vtbl+0x50 = 0x6B4C60) es un getOrCreate que ALOCA un nodo y MUTA el map en fallo de caché
// (no es read-only ni thread-safe). En su lugar replicamos SOLO la rama de LECTURA del lookup,
// con dos métodos independientes que se validan mutuamente en runtime:
//   • Plan A: reproduce el hash de puntero MSVC, indexa el bucket (FR+0x58) y recorre la cadena.
//   • Plan B: barrido lineal de la lista interna del map (centinela en (FR+0x20)), comparando
//     la clave Faction* de cada nodo. Más robusto (no depende del hash). RECOMENDADO.
// Layout del nodo: +0x00 next, +0x08 hash, +0x10 clave Faction*, +0x18 float relación.
// SOLO LECTURA, todo bajo SEH. POD puro (sin objetos C++) para vivir dentro de __try (C2712).
// EN: ── HostilityDiagSnapshot (Phase 4 — DIAG-HOSTILITY) ── DEFINITIVE TEST of why co-op combat is
//     frozen SYMMETRICALLY (the host does not attack bandits and bandits do not attack it). Byte RE
//     (Steam 1.0.68, 2026-06-18, independent double check):
//     Kenshi hostility is NOT decided by the "fundamental type" (OT_CIVILIAN=4) — this REFUTES audit-05.
//     It is decided by an EXPLICIT FLOAT RELATION between Faction objects. The core is isEnemy (RVA
//     0x6B26D0): bool isEnemy(FactionRelations* (Faction+0x78), Faction* other) → (relation <= -30.0).
//     24 combat/AI callers use it BEFORE aggro; none checks the type first. Thresholds: <= -30.0 ENEMY,
//     >= +50.0 ALLY, in between NEUTRAL.
//     A freshly cloned Faction (the "Player N" ModGen copies from "Player 1") is born with an EMPTY
//     relation map; if the clone did not inherit the entries toward vanilla hostile factions, every
//     relation resolves to 0.0 (neutral) → isEnemy=false and isAlly=false with EVERYONE → symmetric
//     frozen combat. Candidate root cause.
//     isEnemy cannot be called from the DIAG (per this block): its internal lookup (getRelationEntry,
//     vtbl+0x50 = 0x6B4C60) is a getOrCreate that ALLOCATES a node on a cache miss. Instead only the READ
//     branch is replicated with two methods that cross-check at runtime: Plan A (MSVC pointer hash →
//     bucket FR+0x58 → chain) and Plan B (linear sweep of the map internal list from the sentinel at
//     FR+0x20). Node layout: +0x00 next, +0x08 hash, +0x10 key Faction*, +0x18 relation float.
//     READ ONLY, all under SEH, pure POD (C2712).
//     NOTE (inconsistency): further down, SEH_CallIsEnemy / SEH_ReadHostilityDiag DO call the native
//     isEnemy, stating it does not allocate (the allocating getOrCreate would be a different function,
//     0x6B4C60); and the map turned out to be a boost::unordered_map, not an MSVC std::_Hash.
// ES: NOTA (incoherencia): más abajo SEH_CallIsEnemy / SEH_ReadHostilityDiag SÍ llaman al isEnemy
//     nativo, afirmando que no aloca (el getOrCreate que aloca sería otra función, 0x6B4C60); y el map
//     resultó ser un boost::unordered_map, no un std::_Hash de MSVC.

// Lookup read-only de relación facción->facción por BARRIDO LINEAL de la lista del map (Plan B).
// fr = FactionRelations* (Faction+0x78). other = Faction* destino. Devuelve la relación float
// (0.0 si no hay entrada = neutro, exactamente como hace el motor al no encontrarla). 'found'
// se pone a true si la entrada existía. SEH-aislado.
// EN: Read-only faction->faction relation lookup by LINEAR SWEEP of the map list (Plan B).
//     fr = FactionRelations* (Faction+0x78), other = target Faction*. Returns the relation float (0.0 if
//     there is no entry = neutral, like the engine). 'found' is set to true if the entry existed.
//     SEH-isolated.
static float SEH_LookupRelationLinear(uintptr_t fr, uintptr_t other, bool* found) {
    if (found) *found = false;
    __try {
        if (fr == 0 || other == 0) return 0.0f;
        // Centinela de la lista doblemente enlazada del std::_Hash MSVC: (fr+0x20) = &_Myhead.
        // EN: Sentinel of the MSVC std::_Hash doubly linked list: (fr+0x20) = &_Myhead.
        uintptr_t sentinel = fr + 0x20;
        uintptr_t headNode = 0;
        if (!Memory::Read(sentinel, headNode) || headNode == 0) return 0.0f;
        uintptr_t cur = 0;
        if (!Memory::Read(headNode, cur)) return 0.0f;   // primer nodo = head.next
        for (int g = 0; cur != 0 && cur != headNode && g < 8192; ++g) {
            uintptr_t key = 0;
            if (!Memory::Read(cur + 0x10, key)) break;
            if (key == other) {
                float rel = 0.0f;
                // El float de la relación vive en nodo+0x18(RelationData)+0x4 = nodo+0x1C.
                // ⚠ NOTA (2026-06-19): este recorrido asume un centinela tipo std::_Hash MSVC en
                // FR+0x20, pero el map REAL es boost::unordered_map (iteración por buckets FR+0x58).
                // Por eso este lookup manual es POCO FIABLE y se mantiene SOLO como dato informativo
                // del DIAG. La hostilidad REAL se confirma con isEnemy NATIVO (0x6B26D0), que es la
                // prueba que dicta el motor. No basar decisiones en host2npc/npc2host (lookup manual).
                // EN: The relation float lives at node+0x18(RelationData)+0x4 = node+0x1C.
                //     NOTE (2026-06-19): this walk assumes an MSVC std::_Hash sentinel at FR+0x20, but the REAL map is a
                //     boost::unordered_map (bucket iteration at FR+0x58). So this manual lookup is UNRELIABLE and kept
                //     ONLY as informative DIAG data. Real hostility is confirmed with the NATIVE isEnemy (0x6B26D0).
                //     Do not base decisions on host2npc/npc2host (manual lookup).
                Memory::Read(cur + 0x1C, rel);
                if (found) *found = true;
                return rel;
            }
            uintptr_t next = 0;
            if (!Memory::Read(cur, next)) break;          // siguiente nodo
            cur = next;
        }
        return 0.0f;   // no encontrado = neutro
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}

// Lookup read-only por HASH+BUCKET (Plan A). Replica getRelationEntry 0x6B4C60 sin alocar.
// Se usa solo para CONTRASTAR con el barrido lineal (si discrepan, el offset del nodo o el
// hash necesitan revisión). SEH-aislado.
// EN: Read-only lookup by HASH+BUCKET (Plan A). Replicates getRelationEntry 0x6B4C60 without allocating.
//     Only used to CROSS-CHECK the linear sweep (if they disagree, the node offset or hash need review).
//     SEH-isolated.
static float SEH_LookupRelationHashed(uintptr_t fr, uintptr_t other, bool* found) {
    if (found) *found = false;
    __try {
        if (fr == 0 || other == 0) return 0.0f;
        uint64_t emptyGuard = 0;
        if (!Memory::Read(fr + 0x40, emptyGuard) || emptyGuard == 0) return 0.0f; // map vacío
        // Hash de puntero MSVC (confirmado por bytes en 0x6B4C60 y su gemela de inserción).
        // EN: MSVC pointer hash (confirmed in bytes at 0x6B4C60 and its insertion twin).
        uint64_t p = static_cast<uint64_t>(other);
        uint64_t t = (p >> 3) + p;
        uint64_t c = (t << 0x15) + ~t;
        uint64_t a = ((c >> 0x18) ^ c) * 0x109ULL;
        uint64_t r = ((a >> 0xE)  ^ a) * 0x15ULL;
        uint64_t hash = ((r >> 0x1C) ^ r) * 0x80000001ULL;

        uint64_t bucketCount = 0;
        if (!Memory::Read(fr + 0x38, bucketCount) || bucketCount == 0) return 0.0f;
        uint64_t mask   = bucketCount - 1;
        uint64_t bucket = mask & hash;

        uintptr_t bucketArray = 0;
        if (!Memory::Read(fr + 0x58, bucketArray) || bucketArray == 0) return 0.0f;
        uintptr_t head = 0;
        if (!Memory::Read(bucketArray + bucket * 8, head) || head == 0) return 0.0f;

        uintptr_t cur = 0;
        if (!Memory::Read(head, cur)) return 0.0f;        // primer nodo real del bucket
        for (int g = 0; cur != 0 && g < 4096; ++g) {
            uint64_t nodeHash = 0;
            if (!Memory::Read(cur + 0x08, nodeHash)) break;
            if (nodeHash == hash) {
                uintptr_t key = 0;
                if (Memory::Read(cur + 0x10, key) && key == other) {
                    float rel = 0.0f;
                    Memory::Read(cur + 0x1C, rel);   // float en nodo+0x1C (ver corrección RE arriba)
                    if (found) *found = true;
                    return rel;
                }
            } else {
                if ((mask & nodeHash) != bucket) break;    // salimos del bucket
            }
            uintptr_t next = 0;
            if (!Memory::Read(cur, next)) break;
            cur = next;
        }
        return 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}

// ES: Snapshot del DIAG-HOSTILITY: facción del host y su FactionRelations (Faction+0x78), facción de un
//     NPC vecino de otra facción, relaciones cruzadas en ambos sentidos (lookup manual lineal y por hash),
//     y el isEnemy NATIVO en los dos sentidos + defaultRelation del host (FR+0x60).
// EN: DIAG-HOSTILITY snapshot: host faction and its FactionRelations (Faction+0x78), the faction of a
//     neighbor NPC of another faction, cross relations both ways (manual linear and hashed lookups), and
//     the NATIVE isEnemy both ways + host defaultRelation (FR+0x60).
struct HostilityDiagSnapshot {
    // Umbrales de relación confirmados por bytes (constantes en .rdata del binario).
    // EN: Relation thresholds confirmed in bytes (constants in the binary .rdata).
    static constexpr float kEnemyThreshold = -30.0f; // rel <= -30 => ENEMIGO  (0x16CCD2C)
    static constexpr float kAllyThreshold  =  50.0f; // rel >= +50 => ALIADO   (0x1683170)

    // ── Facción del HOST (la "Player N" clonada por el mod) ──
    // EN: ── HOST faction (the "Player N" cloned by the mod) ── (note: the "+0x28 _Size" / "+0x40 empty"
    //     descriptions are the old MSVC layout; SEH_ReadFactionRelMeta now stores the boost size from FR+0x40
    //     in RelCount and defaultRelation×1000 in the "Empty" field.)
    // ES: (nota: las descripciones "+0x28 _Size" / "+0x40 vacío" son del layout MSVC antiguo;
    //     SEH_ReadFactionRelMeta guarda ahora el tamaño boost de FR+0x40 en RelCount y defaultRelation×1000
    //     en el campo "Empty".)
    uintptr_t hostChar     = 0;   // primaryChar del mod
    uintptr_t hostFaction  = 0;   // GetPlayerFactionDirect() (GW+0x580 -> +0x2A0)  ó char+0x10
    uintptr_t hostFR       = 0;   // hostFaction + 0x78 (FactionRelations*)
    int       hostFROk     = 0;   // 1 si hostFR es un ptr de heap leíble
    int32_t   hostRelCount = -1;  // FactionRelations+0x28 (_Size = nº de relaciones) — -1 = no leído
    uint64_t  hostEmpty    = 1;   // FactionRelations+0x40 (0 = map VACÍO)

    // ── Facción de un NPC vecino de la simlist (control: facción de mundo real, p.ej. bandidos) ──
    // EN: ── Faction of a neighbor simlist NPC (control: real world faction, e.g. bandits) ──
    uintptr_t npcChar      = 0;
    char      npcName[32]  = {0};
    uintptr_t npcFaction   = 0;   // npcChar + 0x10
    uintptr_t npcFR        = 0;   // npcFaction + 0x78
    int       npcFROk      = 0;
    int32_t   npcRelCount  = -1;  // FactionRelations+0x28 del NPC
    uint64_t  npcEmpty      = 1;
    int       npcFound     = 0;

    // ── Relaciones cruzadas (los DOS sentidos = la simetría del síntoma) ──
    // EN: ── Cross relations (BOTH ways = the symmetry of the symptom) ── Plan A (hashed) is kept to
    //     contrast with Plan B (linear).
    float host2npc      = 0.0f;   // ¿el host considera enemigo al NPC?  (hostFR vs npcFaction)
    int   host2npcFound = 0;      // 1 si existía entrada
    float npc2host      = 0.0f;   // ¿el NPC considera enemigo al host?  (npcFR vs hostFaction)
    int   npc2hostFound = 0;
    // Plan A (hashed) para contraste con Plan B (lineal).
    float host2npcHashed = 0.0f;
    float npc2hostHashed = 0.0f;

    // ── isEnemy NATIVO (RVA 0x6B26D0) — la prueba DEFINITIVA de hostilidad en runtime ──
    // El motor decide la hostilidad con esta función (lee FR+0x60 default si no hay entry).
    // -1 = no llamado/excepción, 0 = NO enemigo, 1 = ENEMIGO. Tras el FIX, hostEnemyNpc debe ser 1.
    // EN: ── NATIVE isEnemy (RVA 0x6B26D0) — the DEFINITIVE runtime hostility test ── The engine decides
    //     hostility with this function (reads the FR+0x60 default when there is no entry). -1 = not
    //     called/exception, 0 = NOT enemy, 1 = ENEMY. After the FIX, hostEnemyNpc must be 1.
    int   hostEnemyNpc   = -1;    // isEnemy(hostFR, npcFaction) — ¿el host ve al NPC como enemigo?
    int   npcEnemyHost   = -1;    // isEnemy(npcFR, hostFaction) — ¿el NPC ve al host como enemigo?
    float hostDefaultRel = 0.0f;  // hostFR+0x60 (defaultFactionRelation del host) — tras el FIX debe ser -100
    int   isEnemyCalled  = 0;     // 1 si se pudo invocar isEnemy nativo

    int read = 0; // 1 si el bloque se ejecutó completo
};

// isEnemy nativo (RVA 0x6B26D0): bool __fastcall(FactionRelations* this, Faction* other).
// SOLO LECTURA (NO aloca: el getOrCreate que sí muta es getRelationData 0x6B4C60, función DISTINTA).
// Seguro de llamar en el hilo de lógica. Devuelve -1 si excepción, 0/1 según el motor.
// EN: Native isEnemy (RVA 0x6B26D0): bool __fastcall(FactionRelations* this, Faction* other). READ ONLY
//     (it does NOT allocate: the mutating getOrCreate is getRelationData 0x6B4C60, a DIFFERENT function).
//     Safe to call on the logic thread. Returns -1 on exception, 0/1 as the engine says.
using IsEnemyFn = bool(__fastcall*)(uintptr_t factionRelations, uintptr_t otherFaction);
static int SEH_CallIsEnemy(IsEnemyFn fn, uintptr_t fr, uintptr_t otherFaction) {
    if (fn == nullptr || fr == 0 || otherFaction == 0) return -1;
    __try {
        return fn(fr, otherFaction) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Lee la metadata del map boost de relaciones de una facción. SEH-aislado.
// ✅ CORREGIDO (layout boost): SIZE en FR+0x40 (no +0x28). 'emptyOut' recibe el defaultRelation
// (FR+0x60, float reinterpretado a uint64 para el log) — el valor que isEnemy lee cuando no hay
// entry. (El antiguo "guard de vacío en +0x40" era una mala interpretación del std::_Hash MSVC.)
// EN: Reads the metadata of a faction boost relation map. SEH-isolated. CORRECTED (boost layout): SIZE at
//     FR+0x40 (not +0x28). 'emptyOut' receives the defaultRelation (FR+0x60, float reinterpreted as
//     uint64 ×1000 for the log) — the value isEnemy reads when there is no entry.
static void SEH_ReadFactionRelMeta(uintptr_t faction, uintptr_t* frOut, int* frOkOut,
                                   int32_t* relCountOut, uint64_t* emptyOut,
                                   bool (*isHeap)(uintptr_t)) {
    __try {
        if (faction == 0) return;
        uintptr_t fr = 0;
        if (!Memory::Read(faction + 0x78, fr) || !isHeap(fr)) return;  // Faction+0x78 = FactionRelations*
        *frOut = fr;
        *frOkOut = 1;
        uint64_t sz = 0;
        if (Memory::Read(fr + 0x40, sz) && sz < 0x100000) *relCountOut = static_cast<int32_t>(sz); // boost element_count
        float defRel = 0.0f;
        if (Memory::Read(fr + 0x60, defRel)) *emptyOut = (uint64_t)(int64_t)(defRel * 1000.0f); // defaultRelation×1000 (diag)
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Recorre la simlist (igual que SEH_ReadCombatStructDiag) y rellena el HostilityDiagSnapshot.
// 'modBase' se usa para resolver isEnemy nativo (0x6B26D0) y comprobar la hostilidad como la ve
// el motor (la prueba DEFINITIVA, mejor que el lookup manual del map).
// EN: Walks the simlist (like SEH_ReadCombatStructDiag) and fills HostilityDiagSnapshot. 'modBase'
//     resolves the native isEnemy (0x6B26D0) to check hostility the way the engine sees it (the
//     DEFINITIVE test, better than the manual map lookup).
static void SEH_ReadHostilityDiag(uintptr_t gwObj, void* primaryChar, uintptr_t playerFaction,
                                  uintptr_t modBase, HostilityDiagSnapshot* out) {
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        return true;
    };
    __try {
        uintptr_t host = reinterpret_cast<uintptr_t>(primaryChar);
        out->hostChar = host;
        // Facción del host: PRIMARIA = GetPlayerFactionDirect (GW+0x580->+0x2A0); FALLBACK = char+0x10.
        // EN: Host faction: PRIMARY = GetPlayerFactionDirect (GW+0x580->+0x2A0); FALLBACK = char+0x10.
        out->hostFaction = playerFaction;
        if (out->hostFaction == 0 && host > 0x10000) {
            uintptr_t fac = 0;
            if (Memory::Read(host + 0x10, fac) && isHeap(fac)) out->hostFaction = fac;
        }
        if (out->hostFaction != 0)
            SEH_ReadFactionRelMeta(out->hostFaction, &out->hostFR, &out->hostFROk,
                                   &out->hostRelCount, &out->hostEmpty, isHeap);

        // NPC vecino de la simlist (primer Character != host) — su facción de mundo es el control.
        // EN: Neighbor simlist NPC (first Character != host of ANOTHER faction) — its world faction is the
        //     control.
        if (gwObj != 0) {
            uint64_t count = 0;
            if (Memory::Read(gwObj + 0x770, count) && count > 0) {
                uintptr_t arrayPtr = 0, headIdx = 0;
                if (Memory::Read(gwObj + 0x788, arrayPtr) && arrayPtr > 0x10000 &&
                    Memory::Read(gwObj + 0x768, headIdx)) {
                    uintptr_t node = 0;
                    if (Memory::Read(arrayPtr + headIdx * 8, node)) {
                        const uint32_t kMaxIter = 20000;
                        uint32_t walked = 0;
                        while (node > 0x10000 && walked < kMaxIter && out->npcFound == 0) {
                            walked++;
                            uintptr_t chr = 0;
                            if (Memory::Read(node + 0x10, chr) && isHeap(chr) && chr != host) {
                                uintptr_t npcFac = 0;
                                if (Memory::Read(chr + 0x10, npcFac) && isHeap(npcFac)
                                    && npcFac != out->hostFaction) {
                                    // NPC vecino de OTRA facción (lo interesante: ¿es hostil al host?).
                                    out->npcChar = chr;
                                    out->npcFaction = npcFac;
                                    SEH_ReadCharName(chr, out->npcName, sizeof(out->npcName));
                                    SEH_ReadFactionRelMeta(npcFac, &out->npcFR, &out->npcFROk,
                                                           &out->npcRelCount, &out->npcEmpty, isHeap);
                                    out->npcFound = 1;
                                    break;
                                }
                            }
                            uintptr_t next = 0;
                            if (!Memory::Read(node + 0x00, next)) break;
                            if (next == node) break;
                            node = next;
                        }
                    }
                }
            }
        }

        // ── Relaciones cruzadas en AMBOS sentidos (la simetría del síntoma) ──
        // host -> npc: ¿el host considera enemigo al NPC?  (hostFR contra npcFaction)
        // EN: ── Cross relations BOTH ways (the symmetry of the symptom) ── host -> npc: does the host see the
        //     NPC as an enemy? (hostFR vs npcFaction)
        if (out->hostFROk && out->npcFaction != 0) {
            bool f = false;
            out->host2npc = SEH_LookupRelationLinear(out->hostFR, out->npcFaction, &f);
            out->host2npcFound = f ? 1 : 0;
            bool fh = false;
            out->host2npcHashed = SEH_LookupRelationHashed(out->hostFR, out->npcFaction, &fh);
        }
        // npc -> host: ¿el NPC considera enemigo al host?  (npcFR contra hostFaction)
        // EN: npc -> host: does the NPC see the host as an enemy? (npcFR vs hostFaction)
        if (out->npcFROk && out->hostFaction != 0) {
            bool f = false;
            out->npc2host = SEH_LookupRelationLinear(out->npcFR, out->hostFaction, &f);
            out->npc2hostFound = f ? 1 : 0;
            bool fh = false;
            out->npc2hostHashed = SEH_LookupRelationHashed(out->npcFR, out->hostFaction, &fh);
        }

        // ── PRUEBA DEFINITIVA: isEnemy NATIVO (lo que el motor realmente evalúa en el gate) ──
        // Mucho más fiable que el lookup manual del map (que asumía layout MSVC). isEnemy lee el
        // default FR+0x60 cuando no hay entry, así que tras el FIX (default=-100) debe dar 1.
        // EN: ── DEFINITIVE TEST: NATIVE isEnemy (what the engine really evaluates at the gate) ── Much more
        //     reliable than the manual map lookup (which assumed an MSVC layout). isEnemy reads the FR+0x60
        //     default when there is no entry, so after the FIX (default=-100) it must return 1.
        if (modBase != 0) {
            IsEnemyFn isEnemy = reinterpret_cast<IsEnemyFn>(modBase + 0x6B26D0);
            if (out->hostFROk && out->npcFaction != 0) {
                out->hostEnemyNpc = SEH_CallIsEnemy(isEnemy, out->hostFR, out->npcFaction);
                out->isEnemyCalled = 1;
            }
            if (out->npcFROk && out->hostFaction != 0) {
                out->npcEnemyHost = SEH_CallIsEnemy(isEnemy, out->npcFR, out->hostFaction);
            }
            // defaultRelation del host (FR+0x60) — confirma si el FIX lo dejó en -100.
            // EN: Host defaultRelation (FR+0x60) — confirms whether the FIX left it at -100.
            if (out->hostFROk) Memory::Read(out->hostFR + 0x60, out->hostDefaultRel);
        }
        out->read = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ══════════════════════════════════════════════════════════════════════════════════════
//  FIX-HOSTILITY (Fase 4) — REPLANTEADO 2026-06-19: defaultRelation negativo en "Player N"
// ══════════════════════════════════════════════════════════════════════════════════════
// CAUSA RAÍZ (CONFIRMADA, triple verificación RE de bytes Steam 1.0.68):
//   La hostilidad en Kenshi la decide una RELACIÓN FLOAT entre Faction objects. El gate de
//   encolado del ataque pregunta isEnemy(miFaction.relations, objetivo); si no es hostil, NO
//   encola la orden → el char ni se mueve. Las facciones de jugador "Player N" que ModGen clona
//   nacen con su map de relaciones VACÍO Y con defaultFactionRelation = 0.0 → isEnemy resuelve
//   a "no enemigo" con TODO el mundo → el host no ataca a nadie y nadie le ataca.
//
// POR QUÉ FALLÓ EL FIX ANTERIOR (log 02:58: esNameless=0, relCountFuente=basura, escritas=0):
//   (1) Asumía que el map era un std::_Hash de MSVC (centinela @FR+0x20, _Size @FR+0x28, nodo
//       key@+0x10/float@+0x1C). Es FALSO: es un boost::unordered_map (size @FR+0x40, buckets
//       @FR+0x58). El recorrido leía basura → relCountFuente=709278728, escritas=0.
//   (2) Buscaba una facción "Player N" poblada como fuente: NO existe (todas nacen vacías).
//   (3) SetControlledChar (0x802520, el FIX-CONTROL) RESETEA Faction+0x78 (relations) entero al
//       ejecutarse → cualquier relación inyectada ANTES se borraba. (Foot-gun no documentado.)
//
// ENFOQUE NUEVO (el MÁS ROBUSTO, verificado por flujo de bytes en isEnemy 0x6B26D0):
//   Cuando NO hay entry específica para 'other', isEnemy/isAlly leen la relación desde
//   FactionRelations+0x60 (defaultFactionRelation). Si escribimos FR+0x60 = -100.0 en la
//   facción del host, entonces isEnemy(host, X)=true para CUALQUIER X sin entry específica
//   (porque -100 <= -30 → enemigo). Esto desbloquea el gate de ataque del host contra todo el
//   mundo hostil de un plumazo, SIN iterar el map boost, SIN alocar nodos, SIN hook en hot-path.
//   Es una escritura de 4 bytes alineada (FR+0x60, 0x60%4==0) → atómica en x64.
//
//   Ventajas frente a copiar relaciones de Nameless (descartado):
//     • No depende de localizar Nameless (que en runtime no se encontró).
//     • No recorre el contenedor boost (frágil, fue la causa del fallo previo).
//     • Resiliente a versión (solo offsets de CAMPO, que no cambian entre Steam/GOG).
//     • Sobrevive al reset de SetControlledChar SI se aplica DESPUÉS (orden garantizado abajo).
//
// COBERTURA DEL RECÍPROCO (que el mundo agreda al host): las facciones de MUNDO ya tienen su
//   propio defaultFactionRelation vanilla hacia las facciones de jugador, y además fijamos
//   fundamentalType=OT_CIVILIAN(4) en las "Player N" para restaurar la clasificación de aggro.
//   Si en pruebas el auto-aggro recíproco resultara insuficiente, la ampliación natural es un
//   hook de isEnemy (RVA 0x6B26D0; AOB documentado en kenshi-re-memory.md) simétrico — pero ese
//   es un paso opcional; el gate de ataque del host (causa raíz del prompt) lo cura ya el default.
//
// SEGURIDAD/THREAD: se ejecuta en el HILO DE LÓGICA (OnGameTick), UNA vez (guard estático),
//   idempotente (SET de un float, no acumula). NO aloca ni muta contenedores → no puede realocar
//   buckets ni cruzarse con el hilo de facciones en medio de una inserción. Todo bajo SEH, POD.
//   Solo se ESCRIBE sobre facciones de JUGADOR (las "Player N"); a las de mundo solo se las
//   identifica (Nameless) para diagnóstico — NO se las modifica.
// EN: FIX-HOSTILITY (Phase 4) — REDESIGNED 2026-06-19: negative defaultRelation on "Player N".
//     ROOT CAUSE (CONFIRMED, triple byte RE Steam 1.0.68): hostility is a FLOAT RELATION between Faction
//       objects. The attack enqueue gate asks isEnemy(myFaction.relations, target); if not hostile the
//       order is NOT queued → the char does not even move. The "Player N" player factions cloned by
//       ModGen are born with an EMPTY relation map AND defaultFactionRelation = 0.0 → isEnemy says "not
//       an enemy" for EVERYONE → the host attacks nobody and nobody attacks it.
//     WHY THE PREVIOUS FIX FAILED: (1) it assumed an MSVC std::_Hash map; it is a boost::unordered_map
//       (size @FR+0x40, buckets @FR+0x58) → garbage reads, 0 writes; (2) it looked for a populated
//       "Player N" as source: none exists; (3) SetControlledChar (0x802520, FIX-CONTROL) RESETS the whole
//       Faction+0x78 (relations) when it runs → anything injected BEFORE was wiped.
//     NEW APPROACH (verified by byte flow in isEnemy 0x6B26D0): with NO specific entry for 'other',
//       isEnemy/isAlly read FactionRelations+0x60 (defaultFactionRelation). Writing FR+0x60 = -100.0 on
//       the host faction makes isEnemy(host, X)=true for ANY X without a specific entry (-100 <= -30).
//       Unblocks the host attack gate against the whole hostile world at once, WITHOUT walking the boost
//       map, WITHOUT allocating, WITHOUT a hot-path hook. Aligned 4-byte write → atomic on x64.
//       Advantages: no need to find Nameless, no boost container walk, version-resilient (field offsets
//       only), survives the SetControlledChar reset IF applied AFTER it (order guaranteed below).
//     RECIPROCAL COVERAGE (world attacking the host): world factions already have their own vanilla
//       default toward player factions, and fundamentalType=OT_CIVILIAN(4) is set on "Player N" to
//       restore aggro classification. If insufficient, the natural extension is a symmetric isEnemy
//       hook (RVA 0x6B26D0) — optional.
//     SAFETY/THREAD: LOGIC THREAD (OnGameTick), ONCE (static guard), idempotent (SET of a float). Does
//       not allocate nor mutate containers. All under SEH, POD. Only PLAYER factions ("Player N") are
//       WRITTEN; world factions (Nameless) are only identified for diagnosis — NOT modified.

// ── Identificación de la facción "Nameless" (StringId FCS "204-gamedata.base") ─────────────
// REPLANTEAMIENTO 2026-06-19 (RE de bytes, game-reverse-engineer, doble verificación):
//   La FUENTE de relaciones correcta NO es "Player 1" (todas las facciones de jugador "Player N"
//   nacen VACÍAS — el FIX previo abortaba buscando una Player N poblada que no existe). La fuente
//   real es "Nameless" (204-gamedata.base), la facción de mundo hacia la que TODO el mundo tiene
//   cableadas sus relaciones de hostilidad vanilla. Se localiza por el StringId de su GameData,
//   NO por isPlayerIface (Nameless es facción de MUNDO, isPlayerIface==0).
//
// Layout CONFIRMADO en bytes (Steam 1.0.68):
//   Faction.data (GameData*) @ +0x240  →  GameData.stringID (std::string MSVC) @ +0x58 == "204-gamedata.base"
//   Faction.name (std::string MSVC)    @ +0x1A8  == "Nameless"  (display name, ruta de respaldo)
// Evidencia: bucle de comparación en getOrCreateFaction (0x2E77F0) lee data+0x240 → +0x58; el ctor
//   (0x802070) y el bootstrap de Nameless (0x86DB80) escriben name@+0x1A8 y stringID@(data+0x240)+0x58.
// EN: ── Identifying the "Nameless" faction (FCS StringId "204-gamedata.base") ── REDESIGN 2026-06-19:
//     the right relation SOURCE is NOT "Player 1" (all "Player N" are born EMPTY) but "Nameless"
//     (204-gamedata.base), the world faction toward which the whole world has its vanilla hostility
//     wired. Located by its GameData StringId, NOT by isPlayerIface (Nameless is a WORLD faction).
//     Layout CONFIRMED in bytes (Steam 1.0.68): Faction.data (GameData*) @ +0x240 → GameData.stringID
//     (MSVC std::string) @ +0x58 == "204-gamedata.base"; Faction.name (MSVC std::string) @ +0x1A8 ==
//     "Nameless" (display name, fallback path). Evidence: getOrCreateFaction (0x2E77F0), the ctor
//     (0x802070) and the Nameless bootstrap (0x86DB80).

// Lee una std::string MSVC (POD, dentro de __try) en 'addr' a un buffer C, devuelve longitud.
// Layout MSVC x64: buf[16] inline @+0x00 / size @+0x10 / capacity @+0x18 (SSO si capacity<16).
// EN: Reads an MSVC std::string (POD, inside __try) at 'addr' into a C buffer; returns the length.
//     MSVC x64 layout: inline buf[16] @+0x00 / size @+0x10 / capacity @+0x18 (SSO if capacity<16).
static size_t SEH_ReadMsvcStr(uintptr_t addr, char* out, size_t outSz) {
    __try {
        uint64_t length = 0, capacity = 0;
        if (!Memory::Read(addr + 0x10, length) || length == 0 || length > 4096) return 0;
        Memory::Read(addr + 0x18, capacity);
        uintptr_t strData = addr;                    // SSO: buffer inline en el propio objeto
        if (capacity >= 16) {                         // heap: +0x00 es el puntero al buffer
            if (!Memory::Read(addr, strData) || strData == 0) return 0;
        }
        size_t copyLen = (length < outSz - 1) ? static_cast<size_t>(length) : outSz - 1;
        __try {
            memcpy(out, reinterpret_cast<const void*>(strData), copyLen);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        out[copyLen] = '\0';
        return copyLen;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ¿Es 'faction' la facción "Nameless"? Comprueba (1) StringId del GameData (data@+0x240 → +0x58)
// contra "204-gamedata.base" y, como respaldo, (2) el nombre (name@+0x1A8) contra "Nameless".
// SEH-aislado, POD puro. 'dataOff'=0x240, 'nameOff'=0x1A8 (ambos confirmados en bytes).
// EN: Is 'faction' the "Nameless" faction? Checks (1) the GameData StringId (data@+0x240 → +0x58) against
//     "204-gamedata.base" and, as fallback, (2) the name (name@+0x1A8) against "Nameless". SEH-isolated,
//     pure POD. 'dataOff'=0x240, 'nameOff'=0x1A8 (both confirmed in bytes).
static bool SEH_IsNamelessFaction(uintptr_t faction, int dataOff, int nameOff,
                                  bool (*isHeap)(uintptr_t)) {
    if (faction == 0) return false;
    __try {
        char buf[64] = {0};
        // (1) StringId del GameData — el identificador FCS inmutable (método robusto).
        // EN: (1) GameData StringId — the immutable FCS identifier (robust method).
        uintptr_t gameData = 0;
        if (Memory::Read(faction + dataOff, gameData) && isHeap(gameData)) {
            if (SEH_ReadMsvcStr(gameData + 0x58, buf, sizeof(buf)) > 0) {
                if (strcmp(buf, "204-gamedata.base") == 0) return true;
            }
        }
        // (2) Respaldo: display name (por si el StringId no resuelve por offset de GameData).
        // EN: (2) Fallback: display name (in case the StringId does not resolve through the GameData offset).
        buf[0] = '\0';
        if (SEH_ReadMsvcStr(faction + nameOff, buf, sizeof(buf)) > 0) {
            if (strcmp(buf, "Nameless") == 0) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Lee el SIZE (element_count) del map boost de relaciones de una facción: FR+0x40 (no +0x28).
// Solo informativo/diag. SEH-aislado. Devuelve nº de entradas o 0 si no legible/vacío.
// EN: Reads the SIZE (element_count) of a faction boost relation map: FR+0x40 (not +0x28). Informative/
//     diag only. SEH-isolated. Returns the entry count or 0 if unreadable/empty.
static uint32_t SEH_RelCountOf(uintptr_t faction, int relationsOff, int sizeOff,
                               bool (*isHeap)(uintptr_t)) {
    uint32_t rc = 0;
    __try {
        uintptr_t fr = 0;
        if (Memory::Read(faction + relationsOff, fr) && isHeap(fr)) {
            uint64_t sz = 0;
            if (Memory::Read(fr + sizeOff, sz) && sz < 100000) rc = static_cast<uint32_t>(sz);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return rc;
}

// Resultado del FIX para logging fuera del __try.
// EN: FIX result for logging outside the __try: applied, factions seen, player factions, defaults
//     written (-100), fundamentalType fixes (=4 OT_CIVILIAN), and Nameless diagnostics.
struct HostilityFixResult {
    int   applied        = 0;  // 1 si se escribió el default hostil en al menos 1 facción de jugador
    int   factionsSeen   = 0;  // nº de facciones enumeradas (sanity del FactionManager)
    int   playerFactions = 0;  // nº de facciones de jugador detectadas (isPlayerIface!=0)
    int   defaultsWritten = 0; // nº de "Player N" a las que se fijó defaultRelation = -100
    int   typesFixed     = 0;  // nº de "Player N" a las que se fijó fundamentalType=4 (OT_CIVILIAN)
    int   namelessFound  = 0;  // 1 si se localizó Nameless (solo informativo/diag)
    uintptr_t namelessFaction = 0; // Faction* de Nameless si se halló (diag)
    int   namelessRelCount = 0; // relCount de Nameless leído con el layout boost correcto (diag)
};

// Núcleo del FIX (REPLANTEADO 2026-06-19 — enfoque defaultRelation). Enumera FactionManager y,
// para cada facción de JUGADOR ("Player N", isPlayerIface!=0):
//   (1) escribe FactionRelations+0x60 (defaultFactionRelation) = -100.0  → isEnemy(host, X)=true
//       para toda X sin entry específica (verificado en bytes: isEnemy 0x6B26D0 lee FR+0x60 como
//       relación cuando el map no tiene nodo para 'other'; -100 <= -30 → enemigo).
//   (2) fija fundamentalType=4 (OT_CIVILIAN) → restaura la clasificación de aggro ("enemigos huyen").
// Localiza Nameless solo para DIAGNÓSTICO (ya no es crítica; el default no depende de ella). SEH,
// POD puro. Se llama UNA vez desde OnGameTick (hilo de lógica), DESPUÉS del FIX-CONTROL (que
// resetea Faction+0x78); por eso este FIX vuelve a escribir el default sobre el map recién reseteado.
//
// 'sizeOff'=FactionRelations.size (0x40 boost), 'defaultRelOff'=FactionRelations.defaultRelation (0x60).
// EN: Core of the FIX (REDESIGNED 2026-06-19 — defaultRelation approach). Enumerates FactionManager and,
//     for each PLAYER faction ("Player N", isPlayerIface!=0): (1) writes FactionRelations+0x60
//     (defaultFactionRelation) = -100.0 → isEnemy(host, X)=true for every X without a specific entry
//     (isEnemy 0x6B26D0 reads FR+0x60 when the map has no node for 'other'; -100 <= -30 → enemy);
//     (2) sets fundamentalType=4 (OT_CIVILIAN) → restores aggro classification. Nameless is located for
//     DIAGNOSIS only. SEH, pure POD. Called ONCE from OnGameTick (logic thread), AFTER FIX-CONTROL (which
//     resets Faction+0x78), so this FIX rewrites the default on the freshly reset map.
//     'sizeOff' = FactionRelations.size (0x40 boost), 'defaultRelOff' = defaultRelation (0x60).
static void SEH_ApplyHostilityFix(uintptr_t factionMgr, int relationsOff, int isPlayerIfaceOff,
                                  int dataOff, int nameOff, int sizeOff, int defaultRelOff,
                                  HostilityFixResult* res) {
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        return true;
    };
    static constexpr int kMaxFactions = 512;   // facciones totales del mundo
    // EN: Constants: max factions, Faction.fundamentalNPCType offset (0x34), OT_CIVILIAN (4) and the hostile
    //     default (-100, below the -30 isEnemy threshold).
    static constexpr int kFundamentalTypeOff = 0x34;  // Faction.fundamentalNPCType (int32) — CONFIRMADO
    static constexpr int kOTCivilian         = 4;     // OT_CIVILIAN — clasificación de mundo (genera aggro)
    static constexpr float kHostileDefault   = -100.0f; // <= -30 (umbral isEnemy) → enemigo de todo

    __try {
        if (factionMgr == 0) return;
        uint32_t count = 0;
        uintptr_t arrayPtr = 0;
        if (!Memory::Read(factionMgr + 0x08, count) || count == 0 || count > kMaxFactions) return;
        // EN: FactionManager: count at +0x08, Faction* array at +0x10.
        if (!Memory::Read(factionMgr + 0x10, arrayPtr) || !isHeap(arrayPtr)) return;
        res->factionsSeen = static_cast<int>(count);

        int playerFactions  = 0;
        int defaultsWritten = 0;
        int typesFixed      = 0;

        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t f = 0;
            if (!Memory::Read(arrayPtr + (uintptr_t)i * 8, f) || !isHeap(f)) continue;

            uintptr_t pif = 0;
            Memory::Read(f + isPlayerIfaceOff, pif);     // isPlayerIface (Faction+0x250)
            // EN: isPlayerIface (Faction+0x250): non-zero for player-controlled factions.

            if (pif != 0) {
                // ── Facción de JUGADOR ("Player N") → aplicar el FIX ──────────────────────────
                // EN: ── PLAYER faction ("Player N") → apply the FIX ──
                playerFactions++;

                // (1) defaultRelation = -100.0 en su FactionRelations (Faction+0x78 → +0x60).
                //     Escritura de 4 bytes alineada (0x60%4==0) → atómica; no muta el contenedor.
                // EN: (1) defaultRelation = -100.0 in its FactionRelations (Faction+0x78 → +0x60). Aligned 4-byte write
                //     → atomic; does not mutate the container. Idempotent (only if not already hostile).
                uintptr_t fr = 0;
                if (Memory::Read(f + relationsOff, fr) && isHeap(fr)) {
                    float cur = 0.0f;
                    if (Memory::Read(fr + defaultRelOff, cur)) {
                        if (cur > kHostileDefault) {     // idempotente: solo si no es ya hostil
                            if (Memory::Write(fr + defaultRelOff, kHostileDefault))
                                defaultsWritten++;
                        } else {
                            defaultsWritten++;            // ya estaba en -100 (cuenta como aplicado)
                        }
                    }
                }

                // (2) fundamentalType = OT_CIVILIAN(4) (Faction+0x34) → restaura aggro vanilla.
                // EN: (2) fundamentalType = OT_CIVILIAN(4) (Faction+0x34) → restores vanilla aggro.
                int32_t curType = 0;
                if (Memory::Read(f + kFundamentalTypeOff, curType)) {
                    if (curType != kOTCivilian) {
                        if (Memory::Write(f + kFundamentalTypeOff, (int32_t)kOTCivilian))
                            typesFixed++;
                    }
                }
            } else {
                // ── Facción de MUNDO: NO se modifica. Solo se identifica Nameless para diag. ──
                // EN: ── WORLD faction: NOT modified. Only Nameless is identified for diagnosis. ──
                if (res->namelessFound == 0 &&
                    SEH_IsNamelessFaction(f, dataOff, nameOff, isHeap)) {
                    res->namelessFound = 1;
                    res->namelessFaction = f;
                    res->namelessRelCount = static_cast<int>(
                        SEH_RelCountOf(f, relationsOff, sizeOff, isHeap)); // layout boost correcto
                }
            }
        }

        res->playerFactions  = playerFactions;
        res->defaultsWritten = defaultsWritten;
        res->typesFixed      = typesFixed;
        // Éxito = había al menos una facción de jugador y se le escribió el default hostil.
        // EN: Success = at least one player faction existed and got the hostile default.
        res->applied = (playerFactions > 0 && defaultsWritten > 0) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ══════════════════════════════════════════════════════════════════════════════════════
//  FIX-HOSTILITY DIRECTO (REPLANTEADO 2026-06-19, runtime-fix) — escribe en la facción del HOST
// ══════════════════════════════════════════════════════════════════════════════════════
// POR QUÉ EL ENFOQUE ANTERIOR ABORTABA (log runtime 10:47-10:49 "ABORTADO tras 600 intentos:
//   no se halló ninguna facción de jugador (isPlayerIface!=0)"):
//   SEH_ApplyHostilityFix iteraba TODO el FactionManager buscando facciones con
//   Faction+0x250 (isPlayerIface) != 0, y en este gamestart NINGUNA facción del array tiene
//   ese puntero poblado. PERO el [DIAG-PCTRL] del MISMO log confirma que el host SÍ es
//   player-controlled: lo es porque  char.faction(char+0x10) == playerFaction (GW+0x580->+0x2A0,
//   = 0xA19EE220), NO por el flag +0x250 (que está a 0 en este flujo). El criterio de búsqueda
//   por isPlayerIface era, por tanto, EQUIVOCADO para este caso: descartaba la única facción
//   que importaba (la del host) por mirar un flag que aquí no se rellena.
//
// SOLUCIÓN (esta función): NO se busca nada. Se recibe la facción del HOST ya resuelta
//   (Opción B del fix: la 'playerFaction' global de GameWorld que lee DIAG-PCTRL; con fallback
//   Opción A: char.faction = primaryChar+0x10) y se escribe el FIX directamente sobre ELLA:
//     (1) FactionRelations+0x60 (defaultFactionRelation) = -100.0  (Faction+0x78 -> +0x60).
//         Escritura de 4 bytes alineada (0x60%4==0) → atómica en x64. isEnemy(host, X)=true
//         para toda X SIN entry específica (-100 <= -30 → enemigo). Cura el gate de ataque.
//     (2) fundamentalType=4 (OT_CIVILIAN) en Faction+0x34 → restaura la clasificación de aggro.
//   Idempotente (solo SET, no acumula), SEH, POD. Re-aplicar ≥3 ticks lo gestiona el llamador
//   (cubre el reset de Faction+0x78 que hace SetControlledChar 0x802520 / FIX-CONTROL).
//
// ⚠ RIESGO AUTO-ATAQUE DEL SQUAD (documentado): poner defaultRelation=-100 hace al host enemigo
//   de TODA facción sin entry explícita — INCLUIDA potencialmente su PROPIA facción si no hay
//   entry de sí misma. Por eso, ANTES de escribir, comprobamos si en el map de relaciones de la
//   facción del host EXISTE una entry hacia SÍ MISMA (key == la propia facción). Si NO existe,
//   la creamos NO es posible sin alocar nodos boost (frágil), así que en su lugar dejamos
//   constancia en el log (selfEntryFound=0) para que el operador valide si el squad se auto-ataca.
//   El log del runtime indica relCount(+0x40 boost)=105 → la facción YA tiene 105 entries, con lo
//   que es muy probable que la entry propia (y las de mundo conocidas) existan y el default solo
//   afecte a desconocidas. Si en pruebas el squad SE auto-atacara, la mitigación correcta es
//   hookear isEnemy 0x6B26D0 con whitelist de la propia facción (paso opcional, no en este fix).
// EN: FIX-HOSTILITY DIRECT (REDESIGNED 2026-06-19, runtime fix) — writes on the HOST faction.
//     WHY THE PREVIOUS APPROACH ABORTED (runtime log "ABORTED after 600 attempts: no player faction
//       found (isPlayerIface!=0)"): SEH_ApplyHostilityFix scanned the whole FactionManager for factions
//       with Faction+0x250 (isPlayerIface) != 0, and in this game start NONE has it. But [DIAG-PCTRL]
//       confirms the host IS player-controlled because char.faction(char+0x10) == playerFaction
//       (GW+0x580->+0x2A0), NOT because of the +0x250 flag. The isPlayerIface criterion was WRONG here.
//     SOLUTION (this function): search nothing. It receives the already resolved HOST faction (option B:
//       the global GameWorld playerFaction; option A fallback: primaryChar+0x10) and writes the FIX
//       directly on IT: (1) FactionRelations+0x60 = -100.0 (atomic aligned 4-byte write; cures the attack
//       gate); (2) fundamentalType=4 (OT_CIVILIAN) at Faction+0x34. Idempotent (SET only), SEH, POD. The
//       caller re-applies it for ≥3 ticks (covers the Faction+0x78 reset done by SetControlledChar).
//     SQUAD SELF-ATTACK RISK (documented): defaultRelation=-100 makes the host an enemy of EVERY faction
//       without an explicit entry — potentially INCLUDING its OWN faction if there is no self entry.
//       Creating that entry is not possible without allocating boost nodes, so relCount is logged for
//       the operator to validate. The runtime log showed relCount(+0x40 boost)=105, so the self entry most
//       likely exists. If the squad attacked itself, the right mitigation is an isEnemy 0x6B26D0 hook
//       with a whitelist of its own faction (optional).
static void SEH_ApplyHostilityFixDirect(uintptr_t hostFaction, int relationsOff,
                                        int sizeOff, int defaultRelOff,
                                        HostilityFixResult* res) {
    auto isHeap = [](uintptr_t v) -> bool {
        if (v < 0x10000 || v >= 0x00007FFFFFFFFFFF) return false;
        if ((v & 0x7) != 0) return false;
        return true;
    };
    static constexpr int   kFundamentalTypeOff = 0x34;     // Faction.fundamentalNPCType (int32)
    // EN: Constants: Faction.fundamentalNPCType offset (0x34), OT_CIVILIAN (4), hostile default (-100).
    static constexpr int   kOTCivilian         = 4;        // OT_CIVILIAN — clasificación de mundo
    static constexpr float kHostileDefault     = -100.0f;  // <= -30 (umbral isEnemy) → enemigo de todo

    __try {
        if (!isHeap(hostFaction)) return;
        res->factionsSeen   = 1;     // operamos sobre 1 facción concreta (la del host)
        // EN: It operates on 1 concrete faction (the host one), counted as a "player" faction for the log.
        res->playerFactions = 1;     // la del host cuenta como "de jugador" para el logging

        // (1) defaultRelation = -100.0 en FactionRelations (Faction+0x78 → +0x60).
        // EN: (1) defaultRelation = -100.0 in FactionRelations (Faction+0x78 → +0x60).
        uintptr_t fr = 0;
        if (Memory::Read(hostFaction + relationsOff, fr) && isHeap(fr)) {
            // Diagnóstico de riesgo: ¿cuántas entries tiene el map? (FR+0x40 boost element_count).
            //   Si es 0 → el default afecta a TODO el mundo, incluida su propia facción → posible
            //   auto-ataque del squad. Si es >0 → es probable que la entry propia exista.
            // EN: Risk diagnosis: how many entries does the map have? (FR+0x40 boost element_count). 0 → the default
            //     affects the WHOLE world, including its own faction → possible squad self-attack. >0 → the self
            //     entry probably exists. (namelessRelCount is reused as a log field.)
            uint64_t relCount = 0;
            if (Memory::Read(fr + sizeOff, relCount) && relCount < 100000)
                res->namelessRelCount = static_cast<int>(relCount); // reutilizamos el campo para el log

            float cur = 0.0f;
            if (Memory::Read(fr + defaultRelOff, cur)) {
                if (cur > kHostileDefault) {           // idempotente: solo si no es ya hostil
                    if (Memory::Write(fr + defaultRelOff, kHostileDefault))
                        res->defaultsWritten = 1;
                } else {
                    res->defaultsWritten = 1;          // ya estaba en -100 (cuenta como aplicado)
                }
            }
        }

        // (2) fundamentalType = OT_CIVILIAN(4) (Faction+0x34) → restaura aggro vanilla.
        // EN: (2) fundamentalType = OT_CIVILIAN(4) (Faction+0x34) → restores vanilla aggro.
        int32_t curType = 0;
        if (Memory::Read(hostFaction + kFundamentalTypeOff, curType)) {
            if (curType != kOTCivilian) {
                if (Memory::Write(hostFaction + kFundamentalTypeOff, (int32_t)kOTCivilian))
                    res->typesFixed = 1;
            } else {
                res->typesFixed = 1;                    // ya estaba (cuenta como aplicado)
            }
        }

        res->namelessFaction = hostFaction; // reutilizamos el campo para volcar la facción tocada
        // Éxito = se escribió (o ya estaba) el default hostil en la facción del host.
        // EN: namelessFaction is reused to log the touched faction. Success = hostile default written (or
        //     already present) on the host faction.
        res->applied = (res->defaultsWritten > 0) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ══════════════════════════════════════════════════════════════════════════════════════
//  FIX-HOSTILITY-HOOK (Fase 4 — audit-15, REVISADO audit-16 2026-06-19) — hook de 0x6B2630
// ══════════════════════════════════════════════════════════════════════════════════════
// ⚠ CORRECCIÓN DE POLARIDAD (audit-16, RE confirmado byte a byte 2 veces): 0x6B2630 es
//   FactionRelations::isAlly (TRUE = ALIADO), NO "isEnemy enriquecida". Compara relation >= +50.0f
//   (.rdata 0x1683170) y lee un flag alliance en RelationData+0x0; devuelve TRUE para misma facción,
//   flag alliance, o rel>=+50. El audit-15 la etiquetó AL REVÉS → el hook quedó INVERTIDO (devolvía
//   TRUE para "marcar enemigo", pero el encolador lo lee como "es aliado, NO ataques"). ESTO SABOTEABA
//   EL ATAQUE: el host se quedaba amIdle=1 SIEMPRE. Este archivo invierte la polaridad a isAlly real.
//
// POR QUÉ el FIX-HOSTILITY del defaultRelation (FR+0x60=-100) NO BASTA (confirmado runtime 11:01):
//   El host es un CLON 'Player 1' (10-kenshi-online.mod) cuya FactionRelations tiene relCount=105
//   entries EXPLÍCITAS, TODAS con relation=0.00 (neutral). isAlly hace getRelationData(other)
//   (vtbl+0x50, alocador 0x6B4C60) que DEVUELVE UN NODO no-null para esas 105 facciones → lee
//   relation=0 → 0 < +50 → NO aliado, pero TAMPOCO usa el default -100 (que solo se mira sin nodo).
//   El gate de ATAQUE real 0x6744A0 encola SOLO si isAlly=FALSE en sus DOS checks (0x6B2630 y
//   Character::isAlly 0x7923D0). El hook invertido forzaba isAlly=TRUE → bloqueo permanente.
//
// CAUSA: 'Player 1' es un clon VACÍO de relaciones: copió la ESTRUCTURA (105 entries) con valores
//   0.00, SIN heredar las relaciones reales de la facción vanilla del jugador 'Nameless'
//   (204-gamedata.base), que SÍ es hostil con bandidos y neutral con comerciantes.
//
// SEGUNDO GATE (0x7923D0 = Character::isAlly, char vt+0x3F0): el encolador 0x6744A0 lo evalúa también
//   (call [char.vt+0x3F0]; test al,al; jne ret-sin-encolar). NO necesita hook propio: tras sus checks
//   propios (misma facción / mismo squad / alianza dinámica — ninguno aplica host-vs-bandido) DELEGA
//   en el MISMO 0x6B2630. Para host-vs-bandido devuelve FALSE por sí solo → hookear 0x6B2630 cubre la
//   rama de facción de AMBOS gates. (RE confirmado audit-16.)
//
// SOLUCIÓN (HÍBRIDA A→C con whitelist propia — polaridad isAlly CORREGIDA):
//   Hook de 0x6B2630 = FactionRelations::isAlly. Firma: bool __fastcall(FactionRelations* this,
//   Faction* other). En el detour (recordar: TRUE=aliado → NO atacar; FALSE=no-aliado → ATACA):
//     1. FAST-PATH: si this != hostFR (la FactionRelations del host) → llamar al original SIN tocar.
//        El 99% de llamadas son NPC-vs-NPC; no las alteramos → no rompemos el mundo ni el rendimiento.
//     2. WHITELIST PROPIA (requisito duro #1): si other == hostFaction (mi propia facción) →
//        devolver TRUE (ALIADO). El squad del host NUNCA se auto-ataca, pase lo que pase.
//     3. HERENCIA REAL:
//        (A, preferida) si la facción Nameless (204-gamedata.base) existe en el FactionManager y su
//           map tiene relaciones → devolver original(namelessFR, other): el host hereda EXACTAMENTE
//           el isAlly del jugador vanilla (NO-aliado de bandidos → ataca; aliado/neutral de la ciudad).
//        (C, fallback) si Nameless NO existe en el mundo del .mod → clasificar por
//           other->fundamentalType (Faction+0x34): OT_BANDIT(8)/OT_SLAVER(7) → NO-ALIADO (false) →
//           el host los ataca; cualquier otro tipo (TRADER/CIVILIAN/DIPLOMAT/...) → original(this,
//           other) = comportamiento normal. Así NUNCA hacemos hostil a comerciantes/ciudadanos (#2).
//
// REENTRANCIA (seguro, verificado): el detour solo intercepta cuando this==hostFR. Al llamar a
//   s_origCombatGate(namelessFR, other) re-entra en 0x6B2630 con un this DISTINTO (namelessFR), que
//   NO es el hostFR → cae en el fast-path → ejecuta el cuerpo original sin volver al detour. El
//   trampolín de MinHook ejecuta los bytes originales + salta al cuerpo (que NO está hookeado);
//   getRelationData (vtbl+0x50) tampoco está hookeado → no hay recursión. (0x6B2630 NO empieza por
//   'mov rax,rsp' según RE → el trampolín estándar de MinHook es válido, sin MovRaxRsp fix.)
// THREAD-SAFETY: corre en el hilo de lógica (gate de combate/AI tick). hostFR/hostFaction/namelessFR
//   se cachean en atomics; se re-resuelven de forma barata si el cache está a 0 (nueva partida). Todo
//   el detour va bajo SEH: ante cualquier fallo, fallback = original (comportamiento vanilla, no crash).
// COEXISTENCIA: el FIX-HOSTILITY del defaultRelation (FR+0x60=-100) y el DIAG con isEnemy nativo se
//   MANTIENEN como respaldo; este hook es la cura definitiva del gate explícito (entries a 0).
// EN: FIX-HOSTILITY-HOOK (Phase 4 — audit-15, REVISED audit-16 2026-06-19) — hook of 0x6B2630.
//     POLARITY CORRECTION (audit-16, byte RE confirmed twice): 0x6B2630 is FactionRelations::isAlly
//       (TRUE = ALLY), NOT an "enriched isEnemy". It compares relation >= +50.0f (.rdata 0x1683170) and
//       reads an alliance flag at RelationData+0x0; returns TRUE for same faction, alliance flag or
//       rel>=+50. audit-15 labelled it BACKWARDS → the hook was INVERTED and SABOTAGED the attack (host
//       stuck at amIdle=1). This file flips the polarity to the real isAlly.
//     WHY the defaultRelation FIX (FR+0x60=-100) IS NOT ENOUGH (confirmed at runtime): the host is a
//       'Player 1' CLONE whose FactionRelations has 105 EXPLICIT entries, ALL at relation=0.00. isAlly
//       calls getRelationData(other) (vtbl+0x50, allocator 0x6B4C60), which RETURNS a non-null node for
//       those 105 factions → reads 0 → not an ally, but the -100 default is never used either. The real
//       ATTACK gate 0x6744A0 only enqueues if isAlly=FALSE in BOTH checks (0x6B2630 and
//       Character::isAlly 0x7923D0).
//     CAUSE: 'Player 1' is an EMPTY relation clone: it copied the STRUCTURE (105 entries) with 0.00
//       values, WITHOUT inheriting the real relations of the vanilla player faction 'Nameless'.
//     SECOND GATE (0x7923D0 = Character::isAlly, char vt+0x3F0): after its own checks (same faction /
//       same squad / dynamic alliance) it DELEGATES to the same 0x6B2630 → hooking 0x6B2630 covers the
//       faction branch of BOTH gates.
//     SOLUTION (HYBRID A→C with own whitelist — isAlly polarity FIXED): hook of 0x6B2630 =
//       bool __fastcall isAlly(FactionRelations* this, Faction* other). In the detour (TRUE=ally → do NOT
//       attack; FALSE=not ally → ATTACK): 1) FAST PATH: this != hostFR → original untouched (99% of calls
//       are NPC-vs-NPC); 2) OWN WHITELIST: other == hostFaction → TRUE (the squad never attacks itself);
//       3A) REAL INHERITANCE: if Nameless exists with relations → original(namelessFR, other): the host
//       inherits the exact vanilla player isAlly; 3C) FALLBACK: classify by other->fundamentalType
//       (Faction+0x34): OT_BANDIT(8)/OT_SLAVER(7) → not ally → attacked; any other type → original.
//     REENTRANCY (safe): calling s_origCombatGate(namelessFR, other) re-enters 0x6B2630 with this !=
//       hostFR → fast path → no recursion. 0x6B2630 does not start with 'mov rax,rsp' → the standard
//       MinHook trampoline is valid.
//     THREAD SAFETY: logic thread; hostFR/hostFaction/namelessFR cached in atomics, cheaply re-resolved.
//       The whole detour runs under SEH: on failure, fallback = original (vanilla, no crash).
//     COEXISTENCE: the defaultRelation FIX (FR+0x60=-100) and the native isEnemy DIAG are KEPT as backup;
//       this hook is the definitive cure of the explicit gate (entries at 0).

// ES: Firma del gate (isAlly real, ver arriba) y trampolín de MinHook al cuerpo original.
// EN: Gate signature (real isAlly, see above) and MinHook trampoline to the original body.
using CombatGateFn = bool(__fastcall*)(uintptr_t factionRelations, uintptr_t otherFaction);
static CombatGateFn        s_origCombatGate = nullptr;   // trampolín de MinHook (cuerpo original)
static std::atomic<bool>   s_combatGateInstalled{false};

// Cache de la facción del host y de Nameless (resueltos en el hilo de lógica, re-resolubles).
// EN: Cache of the host faction and of Nameless (resolved on the logic thread, re-resolvable).
static std::atomic<uintptr_t> s_cachedHostFaction{0};  // Faction* del host (GW+0x580->+0x2A0)
static std::atomic<uintptr_t> s_cachedHostFR{0};       // hostFaction + 0x78 (FactionRelations*)
static std::atomic<uintptr_t> s_cachedNamelessFR{0};   // Nameless.relations (Faction+0x78) o 0
// [FIX-ENEMY-HOOK] Faction* de Nameless (204-gamedata.base), NO su FactionRelations*. Se cachea
//   junto a s_cachedNamelessFR (misma rama de éxito) porque el hook de isEnemy necesita el puntero
//   de FACCIÓN (no de relaciones) para SUSTITUIR el parámetro 'other' de un NPC que pregunta por el
//   host. Vale 0 mientras Nameless no se haya resuelto (fallback seguro = no intervenir).
// EN: [FIX-ENEMY-HOOK] Nameless Faction* (204-gamedata.base), NOT its FactionRelations*. Cached together
//     with s_cachedNamelessFR because the isEnemy hook needs the FACTION pointer to SUBSTITUTE the 'other'
//     parameter when an NPC asks about the host. 0 while Nameless is unresolved (safe fallback).
static std::atomic<uintptr_t> s_cachedNamelessFaction{0}; // Nameless (Faction*) para sustituir 'other'
static std::atomic<int>       s_namelessResolveState{0}; // 0=sin intentar, 1=hallada, 2=NO existe
// Contadores de diagnóstico (logueo throttled, sin spamear el hot-path).
// EN: Diagnostic counters (throttled logging, no hot-path spam).
static std::atomic<int>       s_gateHostCalls{0};      // nº de llamadas interceptadas con this==hostFR
static std::atomic<int>       s_gateHostileVerdicts{0};// nº de veredictos "NO-ALIADO" (=> el host ATACA) del hook

// Tipos fundamentales de facción (Faction+0x34, CharacterTypeEnum) — confirmado KenshiLib Enums.h.
// EN: Faction fundamental types (Faction+0x34, CharacterTypeEnum) — confirmed KenshiLib Enums.h.
static constexpr int kOT_SLAVER = 7;   // esclavistas — agresivos
static constexpr int kOT_BANDIT = 8;   // bandidos — agresivos

// isHeap compartido (mismas guardas que el resto del módulo).
// EN: Shared isHeap (same guards as the rest of the module).
static inline bool GateIsHeap(uintptr_t v) {
    return (v >= 0x10000 && v < 0x00007FFFFFFFFFFF && (v & 0x7) == 0);
}

// ── Resolución de Nameless (204-gamedata.base) por stringID — SEH, una vez (cacheada) ──
// Itera FactionManager.participants (array plano: count@+0x08, array@+0x10) comparando
// Faction+0x240 (GameData*) → GameData+0x58 (stringID) con "204-gamedata.base". Devuelve la
// FactionRelations* (Faction+0x78) de Nameless, o 0 si no existe. NO modifica nada (solo lectura).
// Nota: ReadKenshiString NO puede vivir dentro de __try (objeto C++), así que la comparación de
//   string se hace fuera; aquí solo recolectamos candidatos Faction* y sus GameData* bajo SEH.
// EN: ── Resolving Nameless (204-gamedata.base) by stringID — SEH, once (cached) ── Walks
//     FactionManager.participants (flat array: count@+0x08, array@+0x10) comparing Faction+0x240
//     (GameData*) → GameData+0x58 (stringID) with "204-gamedata.base". Read only. ReadKenshiString cannot
//     live inside __try (C++ object), so the string compare happens outside; here only the Faction* and
//     GameData* candidates are collected under SEH.
static void SEH_CollectFactionGameDatas(uintptr_t factionMgr, uintptr_t* outFactions,
                                        uintptr_t* outGameDatas, int* outCount, int maxN) {
    __try {
        *outCount = 0;
        if (!GateIsHeap(factionMgr)) return;
        uint32_t count = 0;
        uintptr_t arrayPtr = 0;
        if (!Memory::Read(factionMgr + 0x08, count) || count == 0 || count > 4096) return;
        if (!Memory::Read(factionMgr + 0x10, arrayPtr) || !GateIsHeap(arrayPtr)) return;
        for (uint32_t i = 0; i < count && *outCount < maxN; ++i) {
            uintptr_t fac = 0;
            if (!Memory::Read(arrayPtr + i * 8, fac) || !GateIsHeap(fac)) continue;
            uintptr_t gd = 0;
            if (!Memory::Read(fac + 0x240, gd) || !GateIsHeap(gd)) continue;  // Faction+0x240 = GameData*
            outFactions[*outCount] = fac;
            outGameDatas[*outCount] = gd;
            (*outCount)++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { *outCount = 0; }
}

// Lee FactionRelations* (Faction+0x78) bajo SEH y comprueba que tenga relaciones (element_count>0).
// EN: Reads FactionRelations* (Faction+0x78) under SEH and checks it has relations (element_count>0).
static uintptr_t SEH_GetFactionRelationsWithEntries(uintptr_t faction, bool* hasEntries) {
    if (hasEntries) *hasEntries = false;
    __try {
        uintptr_t fr = 0;
        if (!Memory::Read(faction + 0x78, fr) || !GateIsHeap(fr)) return 0;  // Faction+0x78 = FactionRelations*
        uint64_t elemCount = 0;
        if (Memory::Read(fr + 0x40, elemCount) && elemCount > 0 && elemCount < 0x100000) {
            if (hasEntries) *hasEntries = true;
        }
        return fr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Resuelve Nameless (la facción vanilla del jugador) y cachea su FactionRelations*. Idempotente:
// si ya se resolvió (estado 1) o se descartó (estado 2), no repite. Se llama desde el hilo de lógica.
// EN: Resolves Nameless (the vanilla player faction) and caches its FactionRelations*. Idempotent: if
//     already resolved (state 1) or discarded (state 2), it does not repeat. Called from the logic thread.
static void ResolveNamelessFactionOnce(uintptr_t modBase) {
    if (s_namelessResolveState.load(std::memory_order_acquire) != 0) return; // ya intentado
    if (modBase == 0) return;

    // FactionManager: modBase+0x21345B8 (GameWorld@+0x2134110 + 0x4A8) es un PUNTERO GLOBAL en .data
    // a un lektor<Faction*> (RTTI confirmado en vivo), NO la estructura embebida — hace falta un deref.
    // Mismo patrón que theFactory (offset vecino, -0x08). Verificado en vivo 2026-07-13: sin el deref,
    // count@+0x08 leía un objeto "navmesh" adyacente en .data (basura, rechazada por count>4096) → n=0
    // SIEMPRE, Nameless nunca se resolvía. Con el deref: count=105, array real de Faction* válidos.
    // EN: FactionManager: modBase+0x21345B8 (GameWorld@+0x2134110 + 0x4A8) is a GLOBAL POINTER in .data to a
    //     lektor<Faction*> (RTTI confirmed live), NOT the embedded structure — it needs a deref. Same pattern
    //     as theFactory (neighbor offset, -0x08). Verified live 2026-07-13: without the deref, count@+0x08
    //     read an adjacent "navmesh" object (garbage) → n=0 ALWAYS; with it: count=105, real Faction* array.
    uintptr_t factionMgr = 0;
    if (!Memory::Read(modBase + 0x21345B8, factionMgr) || factionMgr == 0) return; // lektor aún no listo

    // Recolectamos (Faction*, GameData*) bajo SEH; comparamos stringID fuera del __try.
    // EN: Collect (Faction*, GameData*) under SEH; compare stringID outside the __try.
    constexpr int kMax = 512;
    static uintptr_t facs[kMax];
    static uintptr_t gds[kMax];
    int n = 0;
    SEH_CollectFactionGameDatas(factionMgr, facs, gds, &n, kMax);
    if (n == 0) {
        // (c) Log THROTTLED: antes este retorno era 100% silencioso.
        // EN: (c) THROTTLED log: this return used to be 100% silent. FactionManager not populated yet → retry.
        static int s_namelessEmptyTicks = 0;
        ++s_namelessEmptyTicks;
        if (s_namelessEmptyTicks == 1 || s_namelessEmptyTicks % 1800 == 0) {
            spdlog::info("[FIX-HOSTILITY-HOOK] ResolveNamelessFactionOnce: FactionManager "
                         "(modBase+0x21345B8) devuelve n=0 facciones — aún no poblado, "
                         "reintentando cada tick (tick #{})", s_namelessEmptyTicks);
        }
        return;  // FactionManager aún no poblado → reintentar en el próximo tick
    }

    uintptr_t namelessFaction = 0;
    for (int i = 0; i < n; ++i) {
        // GameData+0x58 = stringID (Kenshi std::string). ReadKenshiString maneja SSO/heap bajo SEH.
        // EN: GameData+0x58 = stringID (Kenshi std::string). ReadKenshiString handles SSO/heap under SEH.
        std::string sid = SpawnManager::ReadKenshiString(gds[i] + 0x58);
        if (sid == "204-gamedata.base") { namelessFaction = facs[i]; break; }
    }

    if (namelessFaction != 0) {
        bool hasEntries = false;
        uintptr_t nfr = SEH_GetFactionRelationsWithEntries(namelessFaction, &hasEntries);
        if (nfr != 0 && hasEntries) {
            s_cachedNamelessFR.store(nfr, std::memory_order_release);
            // [FIX-ENEMY-HOOK] Opción A: cachear también el Faction* de Nameless. El hook de isEnemy
            //   sustituirá con este puntero el 'other' cuando un NPC pregunte si el host es su enemigo,
            //   heredando así la hostilidad REAL que el NPC ya tiene hacia la facción vanilla del jugador.
            // EN: [FIX-ENEMY-HOOK] Option A: also cache the Nameless Faction*. The isEnemy hook substitutes 'other'
            //     with it when an NPC asks whether the host is its enemy, inheriting the REAL hostility the NPC
            //     already has toward the vanilla player faction.
            s_cachedNamelessFaction.store(namelessFaction, std::memory_order_release);
            s_namelessResolveState.store(1, std::memory_order_release);  // hallada y útil
            spdlog::info("[FIX-HOSTILITY-HOOK] Nameless (204-gamedata.base) RESUELTA: faction=0x{:X} "
                         "FR=0x{:X} (con relaciones) → Opción A: el host hereda sus relaciones reales.",
                         namelessFaction, nfr);
        } else {
            // Existe pero su map está vacío → no aporta; usamos el fallback por tipo.
            // EN: Exists but its map is empty → useless; use the type fallback.
            s_namelessResolveState.store(2, std::memory_order_release);
            spdlog::info("[FIX-HOSTILITY-HOOK] Nameless hallada (0x{:X}) pero su map de relaciones está "
                         "vacío → Opción C (clasificar por fundamentalType: bandidos/esclavistas=hostil).",
                         namelessFaction);
        }
    } else {
        s_namelessResolveState.store(2, std::memory_order_release);  // no existe en el mundo del .mod
        // EN: Does not exist in the .mod world → option C.
        spdlog::info("[FIX-HOSTILITY-HOOK] Nameless (204-gamedata.base) NO existe en el FactionManager "
                     "({} facciones) → Opción C (clasificar por fundamentalType).", n);
    }
}

// Purga los 3 statics de Nameless para que ResolveNamelessFactionOnce se re-arme desde cero tras
// disconnect/recarga de save. SIN esto: el motor libera/recrea las facciones al cargar un save nuevo,
// pero s_namelessResolveState se queda en 1 (permanente, nadie lo resetea) y s_cachedNamelessFaction/
// FR siguen apuntando a memoria reciclada — los hooks isAlly/isEnemy seguirían "sustituyendo por
// Nameless" contra punteros liberados, con el log diciendo que todo va bien. Mismo patrón que los
// otros 7 Reset*Fix() de este fichero (idempotente, se re-resuelve solo en el siguiente tick).
// EN: Purges the 3 Nameless statics so ResolveNamelessFactionOnce re-arms from scratch after a
//     disconnect/save reload. WITHOUT this, the engine frees/recreates factions when loading a new save,
//     but s_namelessResolveState stays at 1 and the cached Faction/FR keep pointing at recycled memory —
//     the isAlly/isEnemy hooks would keep "substituting Nameless" against freed pointers while the log
//     says all is fine. Same pattern as the other Reset*Fix() in this file (idempotent).
void ResetNamelessResolve() {
    s_namelessResolveState.store(0, std::memory_order_release);
    s_cachedNamelessFaction.store(0, std::memory_order_release);
    s_cachedNamelessFR.store(0, std::memory_order_release);
}

// Lee el fundamentalType de una facción (Faction+0x34) bajo SEH. Devuelve -1 si no legible.
// EN: Reads a faction fundamentalType (Faction+0x34) under SEH. Returns -1 if unreadable.
static int SEH_ReadFundamentalType(uintptr_t faction) {
    __try {
        int32_t t = 0;
        if (Memory::Read(faction + 0x34, t)) return t;
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Núcleo de decisión del detour, todo bajo SEH (POD puro, sin objetos C++ en el __try).
// ⚠ SEMÁNTICA isAlly (CORREGIDA audit-16, 2026-06-19): 0x6B2630 es FactionRelations::isAlly,
//   NO isEnemy. El valor de retorno del hook ES el de isAlly: TRUE = ALIADO (el encolador de
//   ataque 0x6744A0 hace `test al,al; jne ret-sin-encolar` → TRUE BLOQUEA el ataque); FALSE =
//   NO aliado → el encolador procede → ATACA. (RE confirmado byte a byte 2 veces, +50.0f en
//   .rdata 0x1683170, flag alliance@RelationData+0x0.)
// Códigos de retorno (en términos de isAlly, NO de "enemigo"):
//   0 = NO-ALIADO  → el host atacará (deja encolar).
//   1 = SÍ-ALIADO  → el host NO atacará (bloquea: solo para su propia facción / squad).
//   2 = DELEGAR al original isAlly tal cual (this/other normales — NPC-vs-NPC o tipo neutral).
//   3 = HEREDAR: evaluar isAlly original con this=namelessFR (delega en SEH_CallOrigGate fuera).
// EN: Decision core of the detour, all under SEH (pure POD). isAlly SEMANTICS (FIXED audit-16): the hook
//     return value IS the isAlly one: TRUE = ALLY (the attack enqueuer 0x6744A0 does `test al,al; jne
//     return-without-enqueue` → TRUE BLOCKS the attack); FALSE = not ally → it attacks.
//     Return codes: 0 = NOT ALLY (host attacks); 1 = ALLY (host does not attack: own faction/squad only);
//     2 = DELEGATE to the original isAlly as is; 3 = INHERIT: evaluate the original isAlly with
//     this=namelessFR (done by SEH_CallOrigGate outside).
static int SEH_DecideCombatGate(uintptr_t thisFR, uintptr_t other, uintptr_t hostFaction,
                                uintptr_t hostFR, uintptr_t namelessFR, int* outMode) {
    // outMode (diag): 0=fast-path(no host) 1=whitelist propia 2=heredado(A) 3=tipo(C) 4=delegado(C-neutral)
    *outMode = 0;
    __try {
        // (1) FAST-PATH: si no es la FactionRelations del host, delegar sin tocar.
        // EN: (1) FAST PATH: not the host FactionRelations → delegate untouched.
        if (thisFR != hostFR) { *outMode = 0; return 2; }

        // (2) WHITELIST PROPIA: la propia facción del host SÍ es aliada → devolver ALIADO (1) para
        //   bloquear el auto-ataque del squad. (Antes devolvía 0 con semántica isEnemy invertida; con
        //   isAlly real, "no atacar a los míos" = TRUE.)
        // EN: (2) OWN WHITELIST: the host own faction IS an ally → return ALLY (1) to block squad self-attack.
        if (other != 0 && other == hostFaction) { *outMode = 1; return 1; /* ALIADO → no atacar */ }

        // (3A) HERENCIA REAL desde Nameless: delegar a isAlly original con this=namelessFR. YA ES
        //   CORRECTA con la semántica isAlly: Nameless NO es aliado de bandidos (rel<+50) → isAlly=FALSE
        //   → el host ataca; Nameless SÍ neutral/aliado de comerciantes según su rel real → no los ataca.
        //   Guard namelessFR != hostFR: evita redirigir a las mismas 105 entries a 0.00 del clon.
        // EN: (3A) REAL INHERITANCE from Nameless: delegate to the original isAlly with this=namelessFR (Nameless
        //     is not an ally of bandits → FALSE → host attacks; traders stay per their real relation). Guard
        //     namelessFR != hostFR avoids redirecting to the clone 105 entries at 0.00.
        if (namelessFR != 0 && namelessFR != hostFR) { *outMode = 2; return 3; /* usar namelessFR */ }

        // (3C) FALLBACK por fundamentalType: bandidos/esclavistas → NO-ALIADO (0) para que el host los
        //   ataque; resto (comerciante/civil/...) → delegar al isAlly original (comportamiento normal).
        // EN: (3C) FALLBACK by fundamentalType: bandits/slavers → NOT ALLY (0) so the host attacks them; the rest
        //     (trader/civilian/...) → delegate to the original isAlly.
        int ft = SEH_ReadFundamentalType(other);
        if (ft == kOT_BANDIT || ft == kOT_SLAVER) { *outMode = 3; return 0; /* NO-ALIADO → atacar */ }
        *outMode = 4;
        return 2;  // tipo neutral (comerciante/civil/...) → comportamiento normal del original
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outMode = 0;
        return 2;  // ante fallo, delegar al original (vanilla)
    }
}

// Llama al original bajo SEH (para la rama heredada A: this=namelessFR). Devuelve false si peta.
// EN: Calls the original under SEH (for inherited branch A: this=namelessFR). Returns false on crash.
static bool SEH_CallOrigGate(uintptr_t thisFR, uintptr_t other) {
    if (s_origCombatGate == nullptr) return false;
    __try {
        return s_origCombatGate(thisFR, other);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── DETOUR del gate de combate 0x6B2630 ──
// bool __fastcall isEnemy_combatGate(FactionRelations* this, Faction* other)
// ES: (El nombre "isEnemy_combatGate" es heredado: la función hookeada es en realidad
//     FactionRelations::isAlly, ver la corrección de polaridad arriba.)
// EN: ── DETOUR of the combat gate 0x6B2630 ── (The "isEnemy_combatGate" name is legacy: the hooked
//     function is actually FactionRelations::isAlly, see the polarity correction above.)
static bool __fastcall Hook_CombatGate(uintptr_t thisFR, uintptr_t other) {
    // Cache del host (resuelto por el FIX-HOSTILITY; aquí solo se LEE, atómico).
    // EN: Host cache (resolved by FIX-HOSTILITY; only READ here, atomic).
    uintptr_t hostFR      = s_cachedHostFR.load(std::memory_order_acquire);
    uintptr_t hostFaction = s_cachedHostFaction.load(std::memory_order_acquire);

    // Si el cache del host aún no está listo, delegamos sin alterar nada (arranque/transición).
    // EN: If the host cache is not ready yet, delegate without changing anything (startup/transition).
    if (hostFR == 0) return SEH_CallOrigGate(thisFR, other);

    uintptr_t namelessFR = s_cachedNamelessFR.load(std::memory_order_acquire);

    int mode = 0;
    int decision = SEH_DecideCombatGate(thisFR, other, hostFaction, hostFR, namelessFR, &mode);

    // ⚠ 'result' ES el retorno de isAlly: TRUE = ALIADO (bloquea ataque), FALSE = NO-aliado (ataca).
    // EN: 'result' IS the isAlly return: TRUE = ALLY (blocks the attack), FALSE = not ally (attacks).
    bool result;
    if (decision == 3) {
        // Opción A: heredar relaciones reales → evaluar el isAlly original con this = Nameless.relations.
        // (Re-entra en 0x6B2630 con this=namelessFR != hostFR → cae en fast-path, sin recursión.)
        // EN: Option A: inherit real relations → evaluate the original isAlly with this = Nameless.relations
        //     (re-enters 0x6B2630 with this=namelessFR != hostFR → fast path, no recursion).
        result = SEH_CallOrigGate(namelessFR, other);
    } else if (decision == 2) {
        result = SEH_CallOrigGate(thisFR, other);  // delegar tal cual (NPC-vs-NPC o tipo neutral)
    } else {
        result = (decision == 1);                  // veredicto directo: 1=ALIADO, 0=NO-aliado
    }

    // Diagnóstico throttled: solo cuando intervenimos sobre el host (mode != 0), cada N veredictos.
    // EN: Throttled diagnostics: only when we intervene on the host (mode != 0), every N verdicts; counts
    //     ATTACK verdicts (not ally = the host will attack). Expected: bandit → NOT ALLY (attacks), own
    //     faction → ALLY (does not attack).
    if (mode != 0) {
        int c = s_gateHostCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        // Contamos veredictos de ATAQUE (no-aliado = el host atacará) — métrica del fix.
        if (!result) s_gateHostileVerdicts.fetch_add(1, std::memory_order_relaxed);
        if (c <= 8 || (c % 200) == 0) {
            const char* modeStr = (mode == 1) ? "whitelist-propia" :
                                  (mode == 2) ? "heredado-Nameless(A)" :
                                  (mode == 3) ? "tipo-bandido/esclavista(C)" : "tipo-neutral(C)";
            // VERIFICACIÓN: con el fix, host->bandido debe loguear isAlly=FALSE (=> NO-aliado => ataca).
            spdlog::info("[FIX-HOSTILITY-HOOK] gate(isAlly) host->other=0x{:X} modo={} isAlly={} "
                         "=> host {} (esperado: bandido=ATACA/no-aliado, propia-faccion=NO-ataca/aliado) "
                         "[hostCalls={}]",
                         other, modeStr, result ? "ALIADO" : "NO-ALIADO",
                         result ? "NO-ataca" : "ATACA", c);
        }
    }
    return result;
}

// Refresca el cache de la facción del host (barato: cadena GW+0x580->+0x2A0 + Faction+0x78).
// Lo llama el FIX-HOSTILITY cada vez que resuelve la facción del host, para mantener el hook al día.
// THREAD: tanto este escritor (desde OnGameTick) como el lector (Hook_CombatGate, gate de combate)
//   corren en el HILO DE LÓGICA del juego. El "TOCTOU" entre los dos stores (hostFaction y hostFR)
//   es inocuo: ambos describen la MISMA facción y solo cambian al cargar partida nueva; no hay otro
//   hilo escribiéndolos. Por eso no se empaquetan en un atomic de 128 bits (complejidad innecesaria).
// EN: Refreshes the host faction cache (cheap: chain GW+0x580->+0x2A0 + Faction+0x78). Called by
//     FIX-HOSTILITY every time it resolves the host faction, to keep the hook up to date. THREAD: both
//     this writer (OnGameTick) and the reader (Hook_CombatGate) run on the game LOGIC THREAD; the TOCTOU
//     between the two stores is harmless (both describe the SAME faction and only change on a new game).
static void RefreshHostFactionCache(uintptr_t hostFaction) {
    if (hostFaction == 0) return;
    s_cachedHostFaction.store(hostFaction, std::memory_order_release);
    uintptr_t fr = 0;
    if (Memory::Read(hostFaction + 0x78, fr) && GateIsHeap(fr))
        s_cachedHostFR.store(fr, std::memory_order_release);
}

// ════════════════════════════════════════════════════════════════════════════════════════
//  [FIX-HOSTREL] Corrección de relaciones de facción CORRUPTAS del host (Fase 4 — 2026-07-13)
// ════════════════════════════════════════════════════════════════════════════════════════
// PROBLEMA (RE en vivo, dry-run de SOLO LECTURA confirmado 2026-07-13, confianza ALTA):
//   NINGÚN NPC contraataca al host ("todos huyen") pese a que el host YA tiene la facción Nameless
//   real. La FactionRelations de esa Nameless (que el host COMPARTE — es el MISMO objeto, no un clon
//   aparte) arrastra 105 relaciones EXPLÍCITAS heredadas de un clon corrupto: 37 de ellas deberían
//   ser hostiles (-100) pero están puestas a 0.00 / -10 (neutral). Esas entries explícitas ANULAN el
//   defaultRelation=-100 (FR+0x60), que sí está bien: getRelationData devuelve un nodo NO-null → se
//   lee la relación explícita neutral → el NPC no ve al host como enemigo → no contraataca.
//
// SOLUCIÓN: pasada one-shot re-armable que recorre el boost::unordered_map de la FactionRelations
//   del host (la MISMA que Nameless) y reescribe a -100 las relaciones que DEBEN ser hostiles
//   (bandidos/esclavistas + fauna salvaje real) y que ahora están en neutral (rel > -30).
//
// LAYOUT (verificado en vivo 2026-07-13 — ⚠ 'relation' va en node+0x1C, NO node+0xC como decía una
//   nota antigua):
//   FactionRelations:  bucket_count@FR+0x38, buckets(Node**)@FR+0x58, defaultRelation(float)@FR+0x60,
//                      element_count@FR+0x40, ownerFaction@FR+0x8.
//   Nodo boost::unordered_map<Faction*, RelationData>:
//     next@node+0x0 (⚠ boost comparte UNA cadena 'next' GLOBAL entre TODOS los buckets → hay que
//                    deduplicar por dirección de nodo; si no, se cuenta el mismo nodo miles de veces),
//     key(Faction*)@node+0x10, RelationData@node+0x18 = { alliance(bool)@+0x0, relation(float)@+0x4 }
//     → relation absoluto = node+0x1C.
//   Faction:  fundamentalNPCType(int)@+0x34, _antiSlavery(bool)@+0x8, notARealFaction(bool)@+0x1D0,
//             GameData*@+0x240;  GameData: stringID(std::string)@+0x58.
//
// SEGURIDAD / por qué NO hay un __try propio envolviendo el bucle: cada acceso ya va por
//   Memory::Read/Memory::Write (individualmente bajo SEH — ver memory.h) y ReadKenshiString también
//   es SEH interno (SSO/heap); GateIsHeap descarta punteros basura. Un __try externo sería redundante
//   Y, además, mezclarlo con el std::unordered_set 'visited' (objeto con destructor) en la MISMA
//   función dispara C2712 en MSVC ("cannot use __try in functions that require object unwinding" — la
//   misma restricción documentada en SEH_ShutdownStep). El guard 'visited' + el límite de 5000 saltos
//   por bucket cortan cualquier ciclo de la cadena 'next' compartida de boost.
// HILO: corre en el HILO DE LÓGICA (game thread), llamado desde OnGameTick — NUNCA desde el de red.
// EN: [FIX-HOSTREL] Fix of the host CORRUPTED faction relations (Phase 4 — 2026-07-13).
//     PROBLEM (live RE, READ-ONLY dry run confirmed, HIGH confidence): NO NPC fights back against the host
//       ("everyone flees") even though the host ALREADY has the real Nameless faction. That Nameless
//       FactionRelations (the host SHARES it — same object) carries 105 EXPLICIT relations inherited from
//       a corrupted clone: 37 should be hostile (-100) but are at 0.00 / -10 (neutral). Those explicit
//       entries OVERRIDE defaultRelation=-100 (FR+0x60) → the NPC does not see the host as an enemy.
//     SOLUTION: re-armable one-shot pass that walks the boost::unordered_map of the host FactionRelations
//       (the SAME as Nameless) and rewrites to -100 the relations that MUST be hostile (bandits/slavers +
//       real wildlife) and are now neutral (rel > -30).
//     LAYOUT (verified live 2026-07-13 — 'relation' is at node+0x1C, NOT node+0xC as an old note said):
//       FactionRelations: bucket_count@FR+0x38, buckets(Node**)@FR+0x58, defaultRelation(float)@FR+0x60,
//       element_count@FR+0x40, ownerFaction@FR+0x8. boost node <Faction*, RelationData>: next@node+0x0
//       (boost shares ONE GLOBAL 'next' chain across ALL buckets → nodes must be deduplicated by address),
//       key(Faction*)@node+0x10, RelationData@node+0x18 = { alliance(bool)@+0x0, relation(float)@+0x4 }
//       → absolute relation = node+0x1C. Faction: fundamentalNPCType(int)@+0x34, _antiSlavery(bool)@+0x8,
//       notARealFaction(bool)@+0x1D0, GameData*@+0x240; GameData: stringID(std::string)@+0x58.
//     SAFETY / why there is no own __try around the loop: every access goes through Memory::Read/Write
//       (individually SEH-protected) and ReadKenshiString is SEH internally; GateIsHeap discards garbage
//       pointers. An outer __try would be redundant AND, mixed with the std::unordered_set 'visited'
//       (object with destructor) in the SAME function, triggers MSVC C2712. The 'visited' guard + the
//       5000-hop per-bucket cap break any cycle of the shared boost 'next' chain.
//     THREAD: LOGIC THREAD (game thread), called from OnGameTick — NEVER from the network thread.

// Tipo fundamental "sin tipo" (Faction+0x34). kOT_SLAVER(7)/kOT_BANDIT(8) ya definidos arriba.
// EN: "No type" fundamental type (Faction+0x34). kOT_SLAVER(7)/kOT_BANDIT(8) are defined above.
static constexpr int kOT_NONE = 0;

// ¿Esta facción DEBE ser hostil al host? Criterio verificado (37 correcciones exactas, 0 falsos
// positivos en el dry-run): bandido/esclavista → sí; "sin tipo" pero facción-no-real y NO
// anti-esclavista y con GameData stringID != "nofac" (= fauna salvaje real) → sí; resto → no.
// EN: MUST this faction be hostile to the host? Verified criterion (37 exact fixes, 0 false positives in
//     the dry run): bandit/slaver → yes; "no type" but not-a-real-faction and NOT anti-slavery and with
//     GameData stringID != "nofac" (= real wildlife) → yes; otherwise → no.
static bool ShouldBeHostile(uintptr_t faction) {   // faction = Faction*
    int32_t type = 0;
    if (!Memory::Read(faction + 0x34, type)) return false;
    if (type == kOT_BANDIT || type == kOT_SLAVER) return true;
    if (type == kOT_NONE) {
        uint8_t notReal = 0, antiSlav = 0;
        Memory::Read(faction + 0x1D0, notReal);   // notARealFaction
        Memory::Read(faction + 0x8, antiSlav);    // _antiSlavery
        if (notReal != 0 && antiSlav == 0) {
            uintptr_t gd = 0;
            if (Memory::Read(faction + 0x240, gd) && gd != 0) {
                // ReadKenshiString es SEH interno; reutiliza el helper ya existente del proyecto.
                // "nofac" = placeholder de facción sin GameData de fauna → NO es fauna salvaje real.
                // EN: ReadKenshiString is SEH internally; reuses the project helper. "nofac" = placeholder faction
                //     without wildlife GameData → NOT real wildlife.
                std::string sid = SpawnManager::ReadKenshiString(gd + 0x58);
                if (sid != "nofac") return true;  // fauna salvaje real → hostil
            }
        }
    }
    return false;
}

// Recorre el boost::unordered_map de la FactionRelations del host y reescribe a -100 las relaciones
// que DEBEN ser hostiles y siguen en neutral (rel > -30). hostFR = s_cachedHostFR (ya resuelto y
// cacheado por el FIX-HOSTILITY cada tick; aquí SOLO se REUTILIZA, no se resuelve la cadena de nuevo).
// Devuelve el nº de relaciones corregidas.
// EN: Walks the boost::unordered_map of the host FactionRelations and rewrites to -100 the relations that
//     MUST be hostile and are still neutral (rel > -30). hostFR = s_cachedHostFR (already resolved and
//     cached by FIX-HOSTILITY every tick; only REUSED here). Returns the number of fixed relations.
static int FixHostFactionRelations(uintptr_t hostFR) {
    if (!GateIsHeap(hostFR)) return 0;
    uint64_t bucketCount = 0;
    uintptr_t buckets = 0;
    if (!Memory::Read(hostFR + 0x38, bucketCount) || bucketCount == 0 || bucketCount > 100000) return 0;
    if (!Memory::Read(hostFR + 0x58, buckets) || !GateIsHeap(buckets)) return 0;

    std::unordered_set<uintptr_t> visited;  // dedup por dirección de nodo (cadena 'next' compartida)
    // EN: 'visited' dedups by node address (shared 'next' chain); guard<5000 cuts any cycle/anomalous chain.
    //     Rewrites node+0x1C (relation) to -100 when the key faction (node+0x10) ShouldBeHostile.
    int fixed = 0;
    for (uint64_t i = 0; i < bucketCount; ++i) {
        uintptr_t node = 0;
        if (!Memory::Read(buckets + i * 8, node)) continue;
        // guard<5000: corta cualquier ciclo/cadena anómala; visited: no re-contar nodos compartidos.
        for (int guard = 0; node != 0 && guard < 5000; ++guard) {
            if (!GateIsHeap(node)) break;               // node desalineado/fuera de rango → no dereferenciar
            if (!visited.insert(node).second) break;   // nodo ya recorrido → fin de esta cadena
            uintptr_t target = 0;
            float rel = 0.0f;
            if (Memory::Read(node + 0x10, target) && GateIsHeap(target) &&
                Memory::Read(node + 0x1C, rel) && rel > -30.0f && ShouldBeHostile(target)) {
                if (Memory::Write(node + 0x1C, -100.0f)) fixed++;   // neutral corrupta → hostil real
            }
            uintptr_t next = 0;
            if (!Memory::Read(node + 0x0, next)) break;
            node = next;
        }
    }
    return fixed;
}

// ── Estado one-shot re-armable del [FIX-HOSTREL] (mismo patrón que ResetHostSimSeedFix) ──
// Se aplica UNA vez por carga (cuando corrige >0 relaciones) y se re-arma en disconnect/recarga por
// si el motor recarga las relaciones al cargar un save. Throttle ~1s como red de seguridad.
// EN: ── Re-armable one-shot state of [FIX-HOSTREL] (same pattern as ResetHostSimSeedFix) ── Applied ONCE
//     per load (when it fixes >0 relations) and re-armed on disconnect/reload in case the engine reloads
//     relations when a save loads. ~1 s throttle as a safety net.
static std::atomic<bool> s_hostRelFixed{false};
static int  s_hostRelAttempts = 0;
static auto s_lastHostRelTry = std::chrono::steady_clock::now();
static constexpr int HOST_REL_MAX_ATTEMPTS = 240; // ~varios segundos de reintentos hasta que hostFR exista

// ES: Re-arma el [FIX-HOSTREL] (disconnect / nueva carga).
// EN: Re-arms [FIX-HOSTREL] (disconnect / new load).
void ResetHostFactionRelationsFix() {
    s_hostRelFixed.store(false);
    s_hostRelAttempts = 0;
    s_lastHostRelTry = std::chrono::steady_clock::now();
}

// Orquesta el fix: REUTILIZA s_cachedHostFR (FactionRelations* del host, cacheado por el
// FIX-HOSTILITY — NO se resuelve la cadena aquí). One-shot re-armable + throttle ~1s. Solo host.
// Llamado desde OnGameTick (ambas ramas), en el HILO DE LÓGICA.
// EN: Drives the fix: REUSES s_cachedHostFR (host FactionRelations*, cached by FIX-HOSTILITY — the chain
//     is not resolved here). Re-armable one-shot + ~1 s throttle. Host only. Called from OnGameTick (both
//     branches), on the LOGIC THREAD. fixed==0 → retried on the next throttle.
static void FixHostFactionRelationsTick(Core& core) {
    (void)core;                                                 // firma homogénea con los demás Seed*Tick
    if (s_hostRelFixed.load()) return;                          // ya sembrado esta carga
    if (s_hostRelAttempts >= HOST_REL_MAX_ATTEMPTS) return;     // nos rendimos (hostFR nunca llegó)

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastHostRelTry < std::chrono::milliseconds(1000)) return; // red de seguridad, no cada frame
    s_lastHostRelTry = now;
    s_hostRelAttempts++;

    uintptr_t hostFR = s_cachedHostFR.load(std::memory_order_acquire);
    if (hostFR == 0) return;  // el FIX-HOSTILITY aún no resolvió la facción del host → reintentar

    int fixed = FixHostFactionRelations(hostFR);
    if (fixed > 0) {
        spdlog::info("[FIX-HOSTREL] corregidas {} relaciones de facción a -100 (esperado ~37)", fixed);
        s_hostRelFixed.store(true);   // one-shot: latch tras corregir al menos una relación
    }
    // fixed==0: hostFR recién cacheado / entries aún no legibles → se reintenta en el próximo throttle.
}

// Instala el hook del gate de combate (una vez). Se llama desde OnGameTick tras tener modBase.
// EN: Installs the combat gate hook (once). Called from OnGameTick once modBase is known. Hooks RVA
//     0x6B2630 (FactionRelations::isAlly — the "isEnemy enriquecida" label in the comment below is the old,
//     refuted audit-15 name) with Hook_CombatGate; if it fails it is not retried in a loop.
// ES: Instala el hook del gate de combate (RVA 0x6B2630 = FactionRelations::isAlly; la etiqueta
//     "isEnemy enriquecida" del comentario de abajo es el nombre antiguo, ya refutado, del audit-15).
//     Si falla, no se reintenta en bucle.
static void InstallCombatGateHookOnce(uintptr_t modBase) {
    if (s_combatGateInstalled.load(std::memory_order_acquire)) return;
    if (modBase == 0) return;
    // RVA confirmado Steam 1.0.68 (audit-15): gate de combate (isEnemy enriquecida) = 0x6B2630.
    uintptr_t gateAddr = modBase + 0x6B2630;
    // Deducción automática del template (mismo patrón que combat_hooks/building_hooks):
    // T = tipo función de &Hook_CombatGate; &s_origCombatGate debe ser T** (lo es: CombatGateFn*).
    // EN: Automatic template deduction (same pattern as combat_hooks/building_hooks): T = function type of
    //     &Hook_CombatGate; &s_origCombatGate must be T** (it is: CombatGateFn*).
    bool ok = HookManager::Get().InstallAt("CombatGate", gateAddr,
                                           &Hook_CombatGate, &s_origCombatGate);
    if (ok) {
        s_combatGateInstalled.store(true, std::memory_order_release);
        spdlog::info("[FIX-HOSTILITY-HOOK] hook del gate de combate INSTALADO en 0x{:X} (RVA 0x6B2630). "
                     "El host heredará hostilidad real (Nameless/A o tipo/C). Whitelist propia activa.",
                     gateAddr);
    } else {
        spdlog::warn("[FIX-HOSTILITY-HOOK] FALLO al instalar el hook del gate de combate en 0x{:X} "
                     "(RVA 0x6B2630). El combate seguirá dependiendo solo del defaultRelation.", gateAddr);
        s_combatGateInstalled.store(true, std::memory_order_release); // no reintentar en bucle
    }
}

// ══════════════════════════════════════════════════════════════════════════════════════
//  FIX-ENEMY-HOOK (2026-07-12) — hook de isEnemy (RVA 0x6B26D0 + COMDAT gemela 0x6B25D0)
// ══════════════════════════════════════════════════════════════════════════════════════
// ESPEJO del FIX-HOSTILITY-HOOK (isAlly, 0x6B2630), pero en la DIRECCIÓN CONTRARIA. Confirmado EN
//   VIVO (Cheat Engine + x64dbg sobre el proceso real, 2026-07-12): ningún NPC contraataca al host
//   aunque reciba daño real — solo huyen. El hook de isAlly SOLO cubre "el host pregunta si X es su
//   aliado" (thisFR == hostFR). La dirección OPUESTA — "un NPC pregunta si el HOST es SU enemigo" —
//   la resuelve isEnemy (0x6B26D0 y su copia COMDAT idéntica 0x6B25D0, AMBAS sin hookear), que lee
//   datos nativos ROTOS: la facción del host es un CLON ('Player 1'/'Sinnombre') con 105 relaciones
//   explícitas a 0.00, así que ningún NPC calcula suficiente hostilidad hacia el host para atacar.
//
// Lógica nativa confirmada de isEnemy(FactionRelations* this, Faction* other):
//     if other == null                    -> FALSE
//     if other == this->owner (FR+0x08)   -> FALSE                 // misma facción
//     rel = this->getRelationData(other) (vtbl+0x50) o this->defaultRelation (FR+0x60)
//     return (-30.0f >= rel)                                       // umbral de hostilidad (.rdata)
//
// CURA (SUSTITUCIÓN, igual filosofía que isAlly Opción A — NO forzamos un valor fijo):
//   Cuando un NPC cualquiera (thisFR != hostFR) pregunta si el HOST (other == hostFaction) es su
//   enemigo, llamamos al ORIGINAL con 'other' SUSTITUIDO por la facción VANILLA del jugador
//   (Nameless, 204-gamedata.base). Así el NPC hereda la hostilidad/relación REAL que ya tiene hacia
//   la facción del jugador de siempre, en vez de la basura del clon. thisFR (la facción del NPC,
//   real) NO se toca. Si Nameless aún no está resuelto → delegamos sin tocar (fallback seguro: a
//   diferencia de isAlly NO hay una Opción C fiable para "quién debería considerarme enemigo", así
//   que sin Nameless preferimos un no-op a un valor inventado).
//
// DOS DIRECCIONES COMDAT: isEnemy vive en 0x6B26D0 y en una copia idéntica 0x6B25D0. Se hookean
//   AMBAS con DOS trampolines separados (los trampolines de MinHook son específicos de cada
//   dirección; NO se puede compartir un único puntero de original). La lógica de decisión está
//   factorizada en EnemyGateBody(); cada detour (A/B) solo aporta SU propio trampolín.
//
// REENTRANCIA (seguro): al llamar al original con (thisFR, namelessFaction), thisFR sigue siendo la
//   facción del NPC (!= hostFR) y other pasa a ser Nameless (!= hostFaction) → si re-entrara en
//   cualquier detour caería en el FAST-PATH (other != hostFaction) y delegaría. El cuerpo nativo de
//   isEnemy llama a getRelationData (vtbl+0x50), no hookeado → sin recursión.
// THREAD-SAFETY: corre en el hilo de lógica (mismo que isAlly). host/Nameless se leen de atomics ya
//   poblados por el FIX-HOSTILITY-HOOK. Todo el detour va bajo SEH: ante fallo, fallback = original.
// NOTA: reutiliza CombatGateFn (bool __fastcall(FactionRelations*, Faction*)) — misma firma que isEnemy.
// EN: FIX-ENEMY-HOOK (2026-07-12) — isEnemy hook (RVA 0x6B26D0 + COMDAT twin 0x6B25D0).
//     MIRROR of FIX-HOSTILITY-HOOK (isAlly, 0x6B2630) in the OPPOSITE direction. Confirmed LIVE (Cheat
//       Engine + x64dbg, 2026-07-12): no NPC fights back against the host even when taking real damage —
//       they only flee. The isAlly hook only covers "the host asks whether X is its ally"; the OPPOSITE
//       ("an NPC asks whether the HOST is ITS enemy") is resolved by isEnemy (both addresses unhooked),
//       which reads BROKEN native data: the host faction is a CLONE with 105 explicit relations at 0.00.
//     Native isEnemy logic: other == null → FALSE; other == this->owner (FR+0x08) → FALSE (same faction);
//       rel = getRelationData(other) (vtbl+0x50) or defaultRelation (FR+0x60); return (-30.0f >= rel).
//     CURE (SUBSTITUTION, same philosophy as isAlly option A — no fixed value is forced): when any NPC
//       (thisFR != hostFR) asks whether the HOST (other == hostFaction) is its enemy, call the ORIGINAL
//       with 'other' SUBSTITUTED by the VANILLA player faction (Nameless). The NPC inherits the REAL
//       hostility it already has toward the usual player faction. thisFR is untouched. Without Nameless
//       → delegate untouched (no reliable option C here; a no-op beats an invented value).
//     TWO COMDAT ADDRESSES: isEnemy lives at 0x6B26D0 and at an identical copy 0x6B25D0. BOTH are hooked
//       with SEPARATE trampolines (MinHook trampolines are per address). The decision logic is factored
//       into EnemyGateBody(); each detour (A/B) only supplies its own trampoline.
//     REENTRANCY (safe): the original is called with (thisFR, namelessFaction): other != hostFaction →
//       any re-entry takes the fast path. getRelationData is not hooked → no recursion.
//     THREAD SAFETY: logic thread; host/Nameless read from atomics populated by FIX-HOSTILITY-HOOK; the
//       whole detour runs under SEH (fallback = original). Reuses CombatGateFn (same signature).

// ES: Trampolines de MinHook de las dos direcciones COMDAT de isEnemy, flag de instalación y contadores
//     de diagnóstico (throttled).
// EN: MinHook trampolines of both isEnemy COMDAT addresses, install flag and diagnostic counters
//     (throttled).
static CombatGateFn        s_origEnemyGateA = nullptr;  // trampolín MinHook de 0x6B26D0 (isEnemy)
static CombatGateFn        s_origEnemyGateB = nullptr;  // trampolín MinHook de 0x6B25D0 (COMDAT gemela)
static std::atomic<bool>   s_enemyGateInstalled{false};
// Contadores de diagnóstico (logueo throttled, sin spamear el hot-path).
static std::atomic<int>     s_enemyGateInterceptCalls{0};  // nº de llamadas donde intervenimos (other==host)
static std::atomic<int>     s_enemyGateHostileVerdicts{0}; // nº de veredictos isEnemy=TRUE (el NPC ataca)

// Llama al original bajo SEH usando el trampolín pasado (cada dirección COMDAT tiene el suyo).
// EN: Calls the original under SEH using the given trampoline (each COMDAT address has its own).
static bool SEH_CallEnemyOrig(CombatGateFn origFn, uintptr_t thisFR, uintptr_t other) {
    if (origFn == nullptr) return false;
    __try {
        return origFn(thisFR, other);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Núcleo de decisión del detour de isEnemy, bajo SEH (POD puro, sin objetos C++ en el __try).
// Semántica isEnemy: TRUE = ENEMIGO (el NPC atacará al host); FALSE = no-enemigo.
// Códigos de retorno:
//   1 = VEREDICTO DIRECTO ENEMIGO (TRUE): (a) Vía A (2026-07-13): la facción del NPC (owner de thisFR)
//       es de tipo hostil por diseño (bandido/esclavista/fauna real, criterio ShouldBeHostile); o
//       (b) ESPEJO host->npc (FIX-HOSTREL-MIRROR, 2026-07-14): el HOST (thisFR==hostFR) pregunta por un
//       npc 'other' que ShouldBeHostile → el host lo ve enemigo. Ambos comparten este mismo retorno.
//   2 = DELEGAR al original tal cual (thisFR, other) — no intervenimos.
//   3 = SUSTITUIR: evaluar el original con other = Nameless (thisFR intacto) — la cura.
// EN: isEnemy detour decision core, under SEH (pure POD). isEnemy semantics: TRUE = ENEMY (the NPC will
//     attack the host); FALSE = not an enemy. Return codes:
//       1 = DIRECT ENEMY verdict (TRUE): (a) path A (2026-07-13): the asking NPC faction (owner of thisFR)
//           is hostile by design (bandit/slaver/real wildlife, ShouldBeHostile); or (b) host->npc MIRROR
//           (FIX-HOSTREL-MIRROR, 2026-07-14): the HOST (thisFR==hostFR) asks about an npc 'other' that
//           ShouldBeHostile → the host sees it as an enemy.
//       2 = DELEGATE to the original as is (thisFR, other).
//       3 = SUBSTITUTE: evaluate the original with other = Nameless (thisFR intact) — the cure.
static int SEH_DecideEnemyGate(uintptr_t thisFR, uintptr_t other, uintptr_t hostFaction,
                               uintptr_t hostFR, uintptr_t namelessFaction, int* outMode) {
    // outMode (diag): 0=fast-path(no nos toca) 1=sustitución(Nameless) 2=sin-Nameless(delegado)
    //                 3=tipo-hostil(Vía A: bandido/esclavista/fauna → enemigo directo)
    //                 4=espejo host->npc (FIX-HOSTREL-MIRROR: el host ve enemigo a un npc hostil)
    // EN: outMode (diag): 0=fast path, 1=substitution (Nameless), 2=no Nameless (delegated),
    //     3=hostile type (path A), 4=host->npc mirror.
    *outMode = 0;
    __try {
        // (0) ESPEJO host->npc — [FIX-HOSTREL-MIRROR] (2026-07-14). Es el ESPEJO de la Vía A (paso 3):
        //   allí un NPC de tipo hostil ve enemigo al host; AQUÍ, cuando quien pregunta es el propio
        //   HOST (thisFR == hostFR, una facción de jugador "Player N"/Nameless) sobre un NPC 'other'
        //   que DEBE ser hostil por diseño (ShouldBeHostile: bandido/esclavista/fauna real), forzamos
        //   ENEMIGO. Causa raíz confirmada esta noche: isEnemy(host->npc) devolvía NEUTRAL porque esos
        //   NPC de mundo tienen una entrada NEUTRAL EXPLÍCITA en el boost::unordered_map de relaciones
        //   del host que ANULA el defaultRelation=-100 ya corregido → el host nunca los clasificaba
        //   como enemigos ("ataque en frío"). Va ANTES del fast-path (paso 1) A PROPÓSITO: ese delega
        //   cualquier 'other' != hostFaction, y aquí 'other' ES el npc (!= hostFaction), justo el caso
        //   que el fast-path descartaría. Reutiliza ShouldBeHostile TAL CUAL (sin relajar el criterio)
        //   para NO volver hostiles aliados ni facciones neutrales legítimas. Igual que la Vía A, solo
        //   SUSTITUYE LA RESPUESTA: no escribe memoria de relaciones en esta dirección.
        //   Guards: other != 0 (no-null), other != hostFaction (no preguntar sobre uno mismo),
        //   GateIsHeap(other) (Faction* válida en heap, no basura).
        // EN: (0) host->npc MIRROR — [FIX-HOSTREL-MIRROR] (2026-07-14). Mirror of path A (step 3): when the HOST
        //     itself asks (thisFR == hostFR) about an NPC 'other' that MUST be hostile by design, force ENEMY.
        //     Root cause confirmed: isEnemy(host->npc) returned NEUTRAL because those world NPCs have an EXPLICIT
        //     NEUTRAL entry in the host relation map that OVERRIDES the corrected defaultRelation=-100 → the host
        //     never classified them as enemies ("cold attack"). It goes BEFORE the fast path ON PURPOSE (the fast
        //     path would drop this case). Reuses ShouldBeHostile AS IS; only SUBSTITUTES THE ANSWER, writes no
        //     relation memory. Guards: other != 0, other != hostFaction, GateIsHeap(other).
        if (thisFR == hostFR && other != 0 && other != hostFaction &&
            GateIsHeap(other) && ShouldBeHostile(other)) {
            *outMode = 4;
            return 1;   // ENEMIGO directo (TRUE) → el host ve hostil al NPC (mismo retorno que Vía A)
        }

        // (1) FAST-PATH (cubre el 99% de llamadas): solo nos interesa cuando alguien pregunta
        //   específicamente si el HOST es su enemigo. Cualquier otro 'other' → delegar sin tocar.
        // EN: (1) FAST PATH (99% of calls): only relevant when someone asks specifically whether the HOST is its
        //     enemy. Any other 'other' → delegate untouched.
        if (other == 0 || other != hostFaction) { *outMode = 0; return 2; }

        // (2) El propio host preguntándose por sí mismo (thisFR == hostFR): el check nativo
        //   other == this->owner ya devuelve FALSE correctamente → no hace falta intervenir.
        // EN: (2) The host asking about itself (thisFR == hostFR): the native other == this->owner check already
        //     returns FALSE → no need to intervene.
        if (thisFR == hostFR) { *outMode = 0; return 2; }

        // (3) VÍA A — VEREDICTO DIRECTO POR TIPO DE FACCIÓN (causa raíz confirmada EN VIVO 2026-07-13,
        //   Cheat Engine + x64dbg, no es hipótesis): la SUSTITUCIÓN (paso 4) solo arregla el caso en el
        //   que el NPC HEREDA por Nameless una relación ya hostil; NO cubre el caso genérico en el que
        //   la facción del NPC, POR SU PROPIO TIPO, debería ver hostil al host aunque su tabla de
        //   relaciones diga neutral. Verificado en vivo: Dust Bandits->host=-10.0 y Esclavistas->host=
        //   0.00 (sus PROPIOS defaultRelation, leídos del FactionRelations de la facción del NPC),
        //   ambos por encima del umbral de enemigo (-30) → el NPC nunca clasificaba al host como
        //   enemigo y nunca iniciaba Task de combate. Aquí: si la facción DUEÑA de thisFR (el NPC que
        //   pregunta) es de tipo hostil por diseño (bandido/esclavista, o fauna salvaje real) según el
        //   MISMO criterio que [FIX-HOSTREL] (reutilizamos ShouldBeHostile, sin duplicar la lógica de
        //   clasificación) Y quien pregunta es por el HOST (other == hostFaction), forzamos ENEMIGO.
        //   SUSTITUCIÓN de la RESPUESTA únicamente: NO se escribe memoria de relaciones en esta
        //   dirección (a diferencia de [FIX-HOSTREL], que sí reescribe la dirección host->npc); así no
        //   interferimos con eventos de reputación nativos del motor para la dirección npc->host.
        //   CAVEAT (no bloqueante): la fauna salvaje NO se verificó en vivo con esta evidencia exacta
        //   (solo bandidos y esclavistas); el mecanismo debería ser idéntico por diseño (mismo
        //   ShouldBeHostile), pero queda pendiente de confirmar con el próximo test en vivo.
        //   npcFaction = ownerFaction del FactionRelations que pregunta (thisFR + 0x8; layout
        //   confirmado en [FIX-HOSTREL] y por el check nativo `other == this->owner`).
        // EN: (3) PATH A — DIRECT VERDICT BY FACTION TYPE (root cause confirmed LIVE 2026-07-13): substitution
        //     (step 4) only fixes the case where the NPC INHERITS an already hostile relation through Nameless;
        //     it does not cover an NPC faction that BY ITS OWN TYPE should see the host as hostile even though its
        //     table says neutral. Verified live: Dust Bandits->host=-10.0 and Slavers->host=0.00 (their OWN
        //     defaultRelation), both above the -30 enemy threshold → never attacked. Here: if the faction OWNING
        //     thisFR is hostile by design (same ShouldBeHostile criterion as [FIX-HOSTREL]) AND the question is
        //     about the HOST, force ENEMY. Answer substitution only (no relation writes in this direction, so the
        //     engine native reputation events are not disturbed). CAVEAT: wildlife was not verified live with this
        //     exact evidence (only bandits and slavers). npcFaction = owner of the asking FactionRelations
        //     (thisFR + 0x8).
        {
            uintptr_t npcFaction = 0;   // Faction* dueña de thisFR (POD, seguro dentro del __try)
            if (Memory::Read(thisFR + 0x8, npcFaction) && GateIsHeap(npcFaction) &&
                ShouldBeHostile(npcFaction)) {
                *outMode = 3;
                return 1;   // ENEMIGO directo (TRUE) → el NPC contraataca al host
            }
        }

        // (4) SUSTITUCIÓN (la cura): un NPC cualquiera pregunta por el host. Si Nameless está
        //   resuelto, sustituir 'other' por la facción vanilla del jugador → hereda la hostilidad real.
        //   Guard != hostFaction (defensa en profundidad, simetría con isAlly:6111): por diseño de
        //   ResolveNamelessFactionOnce esto nunca coincide (Nameless se busca por stringID distinto
        //   al del clon del host), pero evita re-sustituir por el propio host si algún día cambiara.
        // EN: (4) SUBSTITUTION (the cure): any NPC asks about the host. If Nameless is resolved, replace 'other'
        //     with the vanilla player faction → inherits the real hostility. Guard != hostFaction is defense in
        //     depth (symmetry with isAlly; the "isAlly:6111" line reference is out of date).
        if (namelessFaction != 0 && namelessFaction != hostFaction) { *outMode = 1; return 3; }

        // (5) Nameless aún no resuelto → fallback seguro: delegar sin tocar (mejor no-op que inventar).
        // EN: (5) Nameless not resolved yet → safe fallback: delegate untouched (a no-op beats inventing).
        *outMode = 2;
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outMode = 0;
        return 2;  // ante fallo, delegar al original (vanilla)
    }
}

// Cuerpo COMÚN de los detours A/B. 'origFn' es el trampolín de la dirección que disparó (cada
// COMDAT tiene el suyo); 'which' es solo etiqueta de log ('A'=0x6B26D0, 'B'=0x6B25D0).
// EN: COMMON body of detours A/B. 'origFn' is the trampoline of the address that fired (each COMDAT has
//     its own); 'which' is just a log label ('A'=0x6B26D0, 'B'=0x6B25D0).
static bool EnemyGateBody(uintptr_t thisFR, uintptr_t other, CombatGateFn origFn, char which) {
    // Cache del host (resuelto por el FIX-HOSTILITY-HOOK; aquí solo se LEE, atómico).
    // EN: Host cache (resolved by FIX-HOSTILITY-HOOK; only READ here, atomic).
    uintptr_t hostFaction = s_cachedHostFaction.load(std::memory_order_acquire);

    // Si el cache del host aún no está listo, delegamos sin alterar nada (arranque/transición).
    // EN: If the host cache is not ready, delegate without changing anything (startup/transition).
    if (hostFaction == 0) return SEH_CallEnemyOrig(origFn, thisFR, other);

    uintptr_t hostFR          = s_cachedHostFR.load(std::memory_order_acquire);
    uintptr_t namelessFaction = s_cachedNamelessFaction.load(std::memory_order_acquire);

    int mode = 0;
    int decision = SEH_DecideEnemyGate(thisFR, other, hostFaction, hostFR, namelessFaction, &mode);

    // 'result' ES el retorno de isEnemy: TRUE = ENEMIGO (el NPC ATACA al host), FALSE = no-enemigo.
    // EN: 'result' IS the isEnemy return: TRUE = ENEMY (the NPC ATTACKS the host), FALSE = not an enemy.
    bool result;
    if (decision == 1) {
        // VÍA A: veredicto directo ENEMIGO por tipo de facción del NPC (bandido/esclavista/fauna).
        //   Solo SUSTITUIMOS LA RESPUESTA — no llamamos al original ni tocamos memoria del juego
        //   (misma filosofía que el resto del sistema isEnemy/isAlly: nunca escritura directa aquí).
        // EN: PATH A: direct ENEMY verdict by NPC faction type. Only THE ANSWER is substituted — the original is
        //     not called and no game memory is touched.
        result = true;
    } else if (decision == 3) {
        // SUSTITUCIÓN: 'other' pasa a ser la facción vanilla Nameless; thisFR (facción real del NPC)
        //   intacto. Re-entra en el original con other != hostFaction → sin recursión al detour.
        // EN: SUBSTITUTION: 'other' becomes the vanilla Nameless faction; thisFR (real NPC faction) intact.
        //     Re-enters the original with other != hostFaction → no recursion into the detour.
        result = SEH_CallEnemyOrig(origFn, thisFR, namelessFaction);
    } else {
        result = SEH_CallEnemyOrig(origFn, thisFR, other);  // delegar tal cual
    }

    // Diagnóstico throttled: solo cuando intervenimos sobre el host (mode != 0), primeros 8 + cada 200.
    // EN: Throttled diagnostics: only when we intervene on the host (mode != 0), first 8 + every 200.
    //     Counts ATTACK verdicts (enemy = the NPC will attack the host).
    if (mode != 0) {
        int c = s_enemyGateInterceptCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        // Contamos veredictos de ATAQUE (enemigo = el NPC atacará al host) — métrica del fix.
        if (result) s_enemyGateHostileVerdicts.fetch_add(1, std::memory_order_relaxed);
        if (c <= 8 || (c % 200) == 0) {
            if (mode == 4) {
                // ESPEJO host->npc: aquí 'thisFR' es el HOST y 'other' es el NPC al que ahora ve hostil.
                //   Log con prefijo propio [FIX-HOSTREL-MIRROR] para distinguirlo de la dirección npc->host.
                // EN: host->npc MIRROR: here 'thisFR' is the HOST and 'other' the NPC it now sees as hostile; own log
                //     prefix [FIX-HOSTREL-MIRROR] to tell it apart from the npc->host direction.
                spdlog::info("[FIX-HOSTREL-MIRROR] gate(isEnemy)[{}] HOST(thisFR)=0x{:X} pregunta por "
                             "npc(other)=0x{:X} (ShouldBeHostile) => forzado ENEMIGO (el host ve hostil "
                             "al NPC) [intercepts={}]", which, thisFR, other, c);
            } else {
                const char* modeStr = (mode == 1) ? "sustitucion-Nameless" :
                                      (mode == 3) ? "tipo-hostil-ViaA(bandido/esclavista/fauna)" :
                                                    "sin-Nameless(delegado)";
                // VERIFICACIÓN: con el fix, un bandido preguntando por el host debe loguear isEnemy=TRUE
                //   (=> el NPC ATACA), heredado de la hostilidad real de Nameless hacia bandidos.
                // EN: CHECK: with the fix, a bandit asking about the host must log isEnemy=TRUE (=> the NPC ATTACKS).
                spdlog::info("[FIX-ENEMY-HOOK] gate(isEnemy)[{}] NPC(thisFR)=0x{:X} other=0x{:X}{} modo={} "
                             "isEnemy={} => NPC {} al host [intercepts={}]",
                             which, thisFR, other,
                             (mode == 1) ? " (sustituido->Nameless)" : "",
                             modeStr, result ? "ENEMIGO" : "NO-ENEMIGO",
                             result ? "ATACA" : "no-ataca", c);
            }
        }
    }
    return result;
}

// ── DETOURS de isEnemy — uno por dirección COMDAT, cada uno con SU propio trampolín ──
// bool __fastcall isEnemy(FactionRelations* this, Faction* other)
// EN: ── isEnemy DETOURS — one per COMDAT address, each with ITS own trampoline ──
static bool __fastcall Hook_EnemyGateA(uintptr_t thisFR, uintptr_t other) {  // 0x6B26D0
    return EnemyGateBody(thisFR, other, s_origEnemyGateA, 'A');
}
static bool __fastcall Hook_EnemyGateB(uintptr_t thisFR, uintptr_t other) {  // 0x6B25D0 (COMDAT gemela)
    return EnemyGateBody(thisFR, other, s_origEnemyGateB, 'B');
}

// Instala el hook de isEnemy en AMBAS direcciones COMDAT (una vez). Se llama desde OnGameTick, al
// lado de InstallCombatGateHookOnce. Cada dirección usa un nombre y un trampolín distintos.
// EN: Installs the isEnemy hook on BOTH COMDAT addresses (once). Called from OnGameTick next to
//     InstallCombatGateHookOnce. Each address uses a different name and trampoline.
static void InstallEnemyGateHookOnce(uintptr_t modBase) {
    if (s_enemyGateInstalled.load(std::memory_order_acquire)) return;
    if (modBase == 0) return;
    // RVAs confirmados EN VIVO (2026-07-12): isEnemy = 0x6B26D0 y su copia COMDAT idéntica 0x6B25D0.
    // EN: RVAs confirmed LIVE (2026-07-12): isEnemy = 0x6B26D0 and its identical COMDAT copy 0x6B25D0.
    uintptr_t addrA = modBase + 0x6B26D0;  // isEnemy principal
    uintptr_t addrB = modBase + 0x6B25D0;  // copia COMDAT idéntica (mismo cuerpo, otra dirección)
    bool okA = HookManager::Get().InstallAt("EnemyGateA", addrA, &Hook_EnemyGateA, &s_origEnemyGateA);
    bool okB = HookManager::Get().InstallAt("EnemyGateB", addrB, &Hook_EnemyGateB, &s_origEnemyGateB);
    // Enable() defensivo: si el prólogo resultara ser 'mov rax,rsp', InstallAt lo dejaría en bypass
    // (passthrough) esperando un Enable() explícito que si no, nunca llegaría. El hermano isAlly
    // (0x6B2630) no lo necesita (prólogo estándar confirmado), pero isEnemy no se ha verificado a
    // nivel de bytes — Enable() sobre un hook ya activo es idempotente (no-op seguro).
    // EN: Defensive Enable(): if the prologue were 'mov rax,rsp', InstallAt would leave it in bypass
    //     (passthrough) waiting for an explicit Enable() that would otherwise never come. The isAlly sibling
    //     (0x6B2630) does not need it (standard prologue confirmed), but isEnemy was not verified at byte
    //     level — Enable() on an already active hook is an idempotent no-op.
    if (okA) HookManager::Get().Enable("EnemyGateA");
    if (okB) HookManager::Get().Enable("EnemyGateB");
    if (okA || okB) {
        spdlog::info("[FIX-ENEMY-HOOK] hook de isEnemy INSTALADO (A@0x{:X}={}, B@0x{:X}={}). Cuando un "
                     "NPC pregunte si el host es su enemigo, hereda la hostilidad real de Nameless.",
                     addrA, okA ? "ok" : "FALLO", addrB, okB ? "ok" : "FALLO");
    } else {
        spdlog::warn("[FIX-ENEMY-HOOK] FALLO al instalar AMBAS direcciones de isEnemy (A@0x{:X}, "
                     "B@0x{:X}). Los NPC seguirán sin contraatacar al host (solo huyen).", addrA, addrB);
    }
    s_enemyGateInstalled.store(true, std::memory_order_release); // no reintentar en bucle
}

// ══════════════════════════════════════════════════════════════════════════════════════
//  DIAG-BEDREQ (2026-07-13) — hook de DIAGNÓSTICO en TaskData::_isRequirementsComplete
// ══════════════════════════════════════════════════════════════════════════════════════
// OBJETIVO: capturar EN VIVO qué requisito GOAP falla realmente cuando el host intenta
//   dormir/levantarse. La RE estática ya identificó los requisitos exactos de cada Task de
//   cama (evidencia de bytes), pero faltaba el dato definitivo: cuál FALLA en runtime. Este
//   hook lo registra sin poner breakpoints (el juego está en uso).
//
// 100% OBSERVACIONAL — CERO cambio de comportamiento:
//   - Llama al original PRIMERO con los MISMOS argumentos (incluido el MISMO puntero
//     failedOn del caller, para que el original escriba el out-param REAL tal cual).
//   - Captura su retorno (bool complete) y lee failedOn DESPUÉS (solo lectura).
//   - Devuelve EXACTAMENTE el retorno del original; NO altera el out-param.
//   - Todo el detour va bajo SEH, igual que el resto del módulo.
//
// Requisitos GOAP de cama (RE 2026-07-13, evidencia de bytes):
//   - USE_BED(0x62) / USE_BED_ORDER(0x102): ALARMS_IN_THE_VICINITY=0,
//     MACHINE_HAS_FREE_OPERATOR_SLOT=1, HAS_ANYTHING_EQUIPPED=0, IS_CARRYING_SOMETHING=0,
//     AT_LOCATION=1.
//   - GET_OUT_OF_BED(0x74) / GET_UP_STAND_UP(0x41): solo CAN_GET_UP=1.
//
// Firma (KenshiLib, confirmada):
//   bool __fastcall(TaskData* this, AI* ai, hand& target, Vector3& location,
//                   /*stack*/ hand& subTarget, bool autoTargetFinder, StateType& failedOn)
//   Las referencias (hand&/Vector3&/StateType&) son punteros en el ABI x64 → void* en el
//   typedef (el compilador coloca subTarget/autoTargetFinder/failedOn en la pila
//   automáticamente al declarar los 7 parámetros; el detour y la llamada al trampolín usan
//   la misma convención, así que no hace falta ensamblar el paso por pila a mano).
//   this->key (TaskType) en TaskData+0x44 (RE 2026-07-13); se lee como int32 porque
//   0x102=258 no cabe en un byte. Prólogo estándar (48 89 5C 24 10 = mov [rsp+10],rbx;
//   NO mov rax,rsp) → MinHook normal, sin fix MovRaxRsp (verificado: AOB único en .text).
// EN: DIAG-BEDREQ (2026-07-13) — DIAGNOSTIC hook on TaskData::_isRequirementsComplete.
//     GOAL: capture LIVE which GOAP requirement really fails when the host tries to sleep/get up. Static
//       RE already identified the exact requirements of each bed Task; the missing data point was which one
//       FAILS at runtime. This hook logs it without breakpoints.
//     100% OBSERVATIONAL — ZERO behavior change: calls the original FIRST with the SAME args (including the
//       SAME failedOn pointer so the original writes the REAL out-param), captures its return, reads
//       failedOn AFTERWARDS (read only), returns EXACTLY the original result. All under SEH.
//     Bed GOAP requirements (RE 2026-07-13): USE_BED(0x62)/USE_BED_ORDER(0x102): ALARMS_IN_THE_VICINITY=0,
//       MACHINE_HAS_FREE_OPERATOR_SLOT=1, HAS_ANYTHING_EQUIPPED=0, IS_CARRYING_SOMETHING=0, AT_LOCATION=1.
//       GET_OUT_OF_BED(0x74)/GET_UP_STAND_UP(0x41): only CAN_GET_UP=1.
//     Signature (KenshiLib): bool __fastcall(TaskData* this, AI* ai, hand& target, Vector3& location,
//       (stack) hand& subTarget, bool autoTargetFinder, StateType& failedOn). References are pointers in
//       the x64 ABI → void* in the typedef. this->key (TaskType) at TaskData+0x44, read as int32 (0x102
//       does not fit a byte). Standard prologue (NOT mov rax,rsp) → normal MinHook.
using IsReqCompleteFn = bool(__fastcall*)(void* thisTask, void* ai, void* target,
                                          void* location, void* subTarget,
                                          bool autoTargetFinder, void* failedOn);
// ES: Trampolín de MinHook, flag de instalación y contador de fallos de requisito de cama registrados.
// EN: MinHook trampoline, install flag and counter of logged bed requirement failures.
static IsReqCompleteFn   s_origBedReq = nullptr;      // trampolín MinHook (cuerpo original)
static std::atomic<bool> s_bedReqInstalled{false};
static std::atomic<int>  s_bedReqFailCount{0};        // nº de fallos de requisito de cama logueados

// TaskTypes de cama (KenshiLib TaskType enum) — SOLO estos disparan el log de diagnóstico.
// EN: Bed TaskTypes (KenshiLib TaskType enum) — ONLY these trigger the diagnostic log.
static constexpr int kTT_USE_BED        = 0x62;   // 98  — meterse en la cama
static constexpr int kTT_USE_BED_ORDER  = 0x102;  // 258 — orden de usar cama
static constexpr int kTT_GET_OUT_OF_BED = 0x74;   // 116 — salir de la cama
static constexpr int kTT_GET_UP_STAND   = 0x41;   // 65  — levantarse / ponerse de pie

// Nombre legible del TaskType de cama (para el log). "?" si no es de cama.
// EN: Readable name of the bed TaskType (for the log). "?" if it is not a bed task.
static const char* BedTaskName(int key) {
    switch (key) {
        case kTT_USE_BED:        return "USE_BED";
        case kTT_USE_BED_ORDER:  return "USE_BED_ORDER";
        case kTT_GET_OUT_OF_BED: return "GET_OUT_OF_BED";
        case kTT_GET_UP_STAND:   return "GET_UP_STAND_UP";
        default:                 return "?";
    }
}
// ¿Es una de las 4 Tasks de cama que nos interesan?
// EN: Is it one of the 4 bed Tasks we care about?
static inline bool IsBedTask(int key) {
    return key == kTT_USE_BED || key == kTT_USE_BED_ORDER ||
           key == kTT_GET_OUT_OF_BED || key == kTT_GET_UP_STAND;
}

// Llama al original bajo SEH con los 7 args idénticos. *ok=false si el trampolín petó.
// POD puro dentro del __try (sin objetos C++) para no romper el desenrollado de excepciones.
// EN: Calls the original under SEH with the 7 identical args. *ok=false if the trampoline crashed. Pure
//     POD inside the __try.
static bool SEH_CallBedReq(void* thisTask, void* ai, void* target, void* location,
                           void* subTarget, bool autoTargetFinder, void* failedOn, bool* ok) {
    if (s_origBedReq == nullptr) { *ok = false; return false; }
    __try {
        bool r = s_origBedReq(thisTask, ai, target, location, subTarget, autoTargetFinder, failedOn);
        *ok = true;
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ok = false;
        return false;
    }
}

// ── DETOUR de TaskData::_isRequirementsComplete (SOLO LOG, NO modifica nada) ──
// Corre en el HILO DE LÓGICA (AI tick), el mismo que los gate hooks de combate — por eso
// spdlog::info directo aquí es seguro (mismo patrón que Hook_CombatGate, detour MinHook normal).
// EN: ── DETOUR of TaskData::_isRequirementsComplete (LOG ONLY, changes nothing) ── Runs on the LOGIC
//     THREAD (AI tick), like the combat gate hooks — so calling spdlog::info directly here is safe.
static bool __fastcall Hook_BedReq(void* thisTask, void* ai, void* target, void* location,
                                   void* subTarget, bool autoTargetFinder, void* failedOn) {
    // (1) Llama al original PRIMERO con los MISMOS args (mismo puntero failedOn del caller):
    //     el original escribe el out-param real tal cual — no lo tocamos.
    // EN: (1) Call the original FIRST with the SAME args (same failedOn pointer): the original writes the real
    //     out-param as is — we do not touch it.
    bool ok = false;
    bool complete = SEH_CallBedReq(thisTask, ai, target, location, subTarget,
                                   autoTargetFinder, failedOn, &ok);

    // (2) SOLO si el requisito FALLÓ (complete==false) y la Task es de cama, logueamos.
    //     Lecturas SEH-safe (Memory::Read, nunca deref ciego): this->key y *failedOn.
    // EN: (2) ONLY if the requirement FAILED (complete==false) and the Task is a bed one, log it. SEH-safe
    //     reads (Memory::Read): this->key and *failedOn.
    if (ok && !complete) {
        int key = -1;
        Memory::Read(reinterpret_cast<uintptr_t>(thisTask) + 0x44, key); // TaskData+0x44 = TaskType (int32)
        if (IsBedTask(key)) {
            int failedOnVal = -1;
            if (failedOn) Memory::Read(reinterpret_cast<uintptr_t>(failedOn), failedOnVal); // StateType (int32)
            int n = s_bedReqFailCount.fetch_add(1, std::memory_order_relaxed) + 1;
            // Throttle: primeros 30 + 1 de cada 30. El AI tick re-evalúa cada tick mientras el
            // requisito siga fallando, así que sin throttle inundaría el log. Se loguea para
            // CUALQUIER AI (no solo el host): determinar aquí si el AI es el del host es costoso
            // (cadena AI→Character→Faction inversa) y ruidoso pero simple; el ai=0x.. del log
            // permite correlacionarlo a posteriori. (Ver reporte del executor.)
            // EN: Throttle: first 30 + 1 in 30 (the AI tick re-evaluates every tick while it keeps failing). Logged
            //     for ANY AI (not only the host): telling whether it is the host is costly; the ai=0x.. in the log
            //     allows correlating afterwards.
            if (n <= 30 || n % 30 == 0) {
                spdlog::info("[DIAG-BEDREQ] #{} taskType=0x{:X}({}) FALLA requisito -> failedOn=0x{:X} "
                             "(StateType) ai=0x{:X} this=0x{:X} [cualquier AI]",
                             n, (unsigned)key, BedTaskName(key), (unsigned)failedOnVal,
                             reinterpret_cast<uintptr_t>(ai), reinterpret_cast<uintptr_t>(thisTask));
            }
        }
    }

    // (3) Devuelve EXACTAMENTE lo que devolvió el original. Si el trampolín petó (ok==false),
    //     complete=false = degradación segura: el caller trata la Task como "requisitos
    //     incompletos", idéntico a tener el hook desactivado. No se altera nada.
    // EN: (3) Return EXACTLY what the original returned. If the trampoline crashed (ok==false),
    //     complete=false = safe degradation, identical to the hook being disabled.
    return complete;
}

// Instala el hook DIAG-BEDREQ (una vez). Se llama desde OnGameTick, junto a los gate hooks.
// EN: Installs the DIAG-BEDREQ hook (once). Called from OnGameTick next to the gate hooks.
static void InstallBedReqDiagHookOnce(uintptr_t modBase) {
    if (s_bedReqInstalled.load(std::memory_order_acquire)) return;
    if (modBase == 0) return;
    // RVA confirmada EN VIVO (2026-07-13) + AOB único en .text (48 89 5C 24 10 4C 89 4C 24 20
    // 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C): 0x60F940. Prólogo estándar → MinHook normal.
    // EN: RVA confirmed LIVE (2026-07-13) + unique AOB in .text: 0x60F940. Standard prologue → normal
    //     MinHook.
    uintptr_t addr = modBase + 0x60F940;
    bool ok = HookManager::Get().InstallAt("BedReqDiag", addr, &Hook_BedReq, &s_origBedReq);
    if (ok) {
        spdlog::info("[DIAG-BEDREQ] hook instalado en TaskData::_isRequirementsComplete 0x{:X} "
                     "(RVA 0x60F940). Al dormir/levantarse el host, loguea el StateType que falla "
                     "para las Tasks de cama (USE_BED/USE_BED_ORDER/GET_OUT_OF_BED/GET_UP_STAND_UP).",
                     addr);
    } else {
        spdlog::warn("[DIAG-BEDREQ] FALLO al instalar el hook en 0x{:X} (RVA 0x60F940) — "
                     "el diagnóstico de requisitos de cama no estará disponible.", addr);
    }
    s_bedReqInstalled.store(true, std::memory_order_release); // no reintentar en bucle
}

// ══════════════════════════════════════════════════════════════════════════════════════
//  [FIX-ARMGATE] (2026-07-13) — hook de MedicalSystem::hasWorkingArm (RVA 0x644150)
//  "Mi brazo está en un estado tan pésimo que no puedo levantar nada" al intentar cargar.
// ══════════════════════════════════════════════════════════════════════════════════════
// CAUSA RAÍZ (RE en vivo con Cheat Engine/x64dbg 2026-07-13, no es hipótesis):
//   hasWorkingArm (RVA 0x644150) es el ÚNICO punto de choque de 7 callers GOAP distintos
//   (atacar, cargar/lift, usar objetos...). Lee dos bytes cacheados en el MedicalSystem inline:
//     char+0x458+0x165 = char+0x5BD = rightArmOk   (1=funcional, 0=roto)
//     char+0x458+0x166 = char+0x5BE = leftArmOk
//   y devuelve 1 si CUALQUIERA es != 0; 0 sólo si AMBOS son 0. AOB único confirmado byte a byte:
//     80 B9 65 01 00 00 00 75 0C 80 B9 66 01 00 00 00 75 03 32 C0 C3 B0 01 C3
//   Esos dos bytes arrancan con la BASURA del clon reclamado por el mod y sólo los recalcula
//   MedicalSystem::reassessCollapseMode. Corre en el PLANIFICADOR GOAP, ANTES de encolar ninguna
//   orden → NINGÚN hook de Hook_AddOrderBackend (incluido [FIX-CARRY-HAND]) lo ve nunca (0 entradas
//   de log de ese fix para este síntoma). El SeedHostCharArmFlagsTick ([FIX-ARMSEED]) re-siembra el
//   bool pero su timing/cobertura no garantiza cubrir el instante exacto del intento de carga.
//
// FIX (Opción A recomendada por la investigación — ataca la causa en el ÚNICO punto de choque en vez
//   de perseguir cada flag por separado): detour sobre hasWorkingArm que, SÓLO para chars de la
//   facción del host/jugador (mismo criterio que el resto del sistema: char.faction(char+0x10) ==
//   s_cachedHostFaction, el atomic ya poblado por RefreshHostFactionCache), sustituye la CONSULTA por
//   DATOS REALES en vez de forzar un booleano a ciegas (misma filosofía que isAlly/isEnemy hoy):
//     · Lee la salud REAL de los dos brazos desde el array de partes de salud (cadena canónica
//       MedicalSystem: partArray@char+0x5F8, count@char+0x5F0, stride 8, flesh@part+0x40 — los MISMOS
//       offsets que SEH_WriteLimbHealthDirect/SEH_ReassessCollapse, no se reinventan). Índices de
//       brazo = kmp::BodyPart (Head=0,Chest=1,Stomach=2,LeftArm=3,RightArm=4,...).
//     · Si al menos un brazo tiene flesh>0 (funcional) PERO el veredicto nativo era 0 → CORRIGE a 1.
//     · Si la salud real también dice brazo roto de verdad (ambos flesh<=0) o no es legible, o el
//       nativo ya daba 1 → delega al veredicto nativo. NUNCA fuerza true sobre un brazo destrozado.
//   Para NPCs y chars remotos (charFaction != s_cachedHostFaction) delega tal cual: no toca IA ajena.
//   COOP: en un cliente, s_cachedHostFaction es la facción del propio jugador → corrige sus propios
//   chars, que es igualmente deseable (mismo razonamiento inofensivo/deseable de [FIX-CARRY-HAND]).
// SEGURIDAD: el detour llama SIEMPRE al original primero (barato, 2 bytes) y va bajo SEH; ante
//   cualquier fallo devuelve el veredicto nativo (degradación = hook desactivado). Prólogo
//   `80 B9 65 01...` (cmp, NO mov rax,rsp) → MinHook normal, sin fix MovRaxRsp.
// HILO: corre en el hilo de lógica (planificador GOAP/AI tick), el mismo que los gate hooks de
//   combate — por eso spdlog directo aquí es seguro (mismo patrón que Hook_CombatGate).
// EN: [FIX-ARMGATE] (2026-07-13) — hook of MedicalSystem::hasWorkingArm (RVA 0x644150). "My arm is in such
//     bad shape that I cannot lift anything" when trying to carry.
//     ROOT CAUSE (live RE with Cheat Engine/x64dbg): hasWorkingArm is the ONLY choke point of 7 GOAP
//       callers (attack, carry/lift, use objects...). It reads two cached bytes in the inline
//       MedicalSystem: char+0x458+0x165 = char+0x5BD = rightArmOk and char+0x458+0x166 = char+0x5BE =
//       leftArmOk, and returns 1 if EITHER is != 0 (unique AOB, see bytes above). Those bytes start with the
//       GARBAGE of the clone claimed by the mod and only MedicalSystem::reassessCollapseMode recomputes
//       them. It runs in the GOAP PLANNER, BEFORE any order is queued → no Hook_AddOrderBackend hook sees
//       it. [FIX-ARMSEED] re-seeds the bool but cannot guarantee covering the exact moment.
//     FIX (option A): detour on hasWorkingArm that, ONLY for chars of the host/player faction
//       (char.faction(char+0x10) == s_cachedHostFaction), replaces the QUERY with REAL DATA instead of
//       forcing a bool blindly: reads the REAL health of both arms from the health part array (partArray@
//       char+0x5F8, count@char+0x5F0, stride 8, flesh@part+0x40 — same offsets as
//       SEH_WriteLimbHealthDirect/SEH_ReassessCollapse; arm indices per kmp::BodyPart: LeftArm=3,
//       RightArm=4). If at least one arm has flesh>0 BUT the native verdict was 0 → CORRECT to 1. If both
//       are really broken, unreadable, or native was already 1 → keep the native verdict. NEVER forces true
//       on a destroyed arm. NPCs and remote chars → delegate untouched. In co-op, on a client,
//       s_cachedHostFaction is the player own faction → it fixes its own chars (desirable).
//     SAFETY: the detour ALWAYS calls the original first (cheap) and runs under SEH; on failure it returns
//       the native verdict. Prologue `80 B9 65 01...` (cmp, NOT mov rax,rsp) → normal MinHook.
//     THREAD: logic thread (GOAP planner / AI tick) → direct spdlog is safe here.

// Firma nativa de MedicalSystem::hasWorkingArm (RVA 0x644150). __fastcall(this=MedicalSystem* en rcx),
// retorno bool en al. this = char+0x458 (MedicalSystem vive INLINE en el Character).
// EN: Native signature of MedicalSystem::hasWorkingArm (RVA 0x644150). __fastcall(this=MedicalSystem* in
//     rcx), bool in al. this = char+0x458 (MedicalSystem lives INLINE inside the Character).
// ES: Trampolín de MinHook, flag de instalación y contador de correcciones del veredicto.
// EN: MinHook trampoline, install flag and counter of verdict corrections.
using HasWorkingArmFn = uint8_t(__fastcall*)(uintptr_t medicalSystem);
static HasWorkingArmFn   s_origHasWorkingArm = nullptr;   // trampolín MinHook (cuerpo original)
static std::atomic<bool> s_armGateInstalled{false};
static std::atomic<int>  s_armGateCorrections{0};         // nº de veces que el hook corrigió el veredicto

// MedicalSystem inline dentro del Character: char = medicalSystem - 0x458 (ver SEH_ReassessCollapse).
// EN: Inline MedicalSystem inside the Character: char = medicalSystem - 0x458 (see SEH_ReassessCollapse).
static constexpr uintptr_t kMedicalSystemOffset = 0x458;
// Índices de parte de salud (mismo orden que kmp::BodyPart y el array health[7] de MsgLimbHealth:
//   Head=0, Chest=1, Stomach=2, LeftArm=3, RightArm=4, LeftLeg=5, RightLeg=6).
// EN: Health part indices (same order as kmp::BodyPart and MsgLimbHealth health[7]: Head=0, Chest=1,
//     Stomach=2, LeftArm=3, RightArm=4, LeftLeg=5, RightLeg=6).
static constexpr int kArmPartIdxLeft  = 3;   // BodyPart::LeftArm
static constexpr int kArmPartIdxRight = 4;   // BodyPart::RightArm

// Llama al original bajo SEH. *ok=false si el trampolín peta (devolvemos 0 = "sin brazo", degradación
// segura idéntica a tener el hook desactivado). POD puro dentro del __try.
// EN: Calls the original under SEH. *ok=false if the trampoline crashes (returns 0 = "no arm", safe
//     degradation identical to the hook being disabled). Pure POD inside the __try.
static uint8_t SEH_CallHasWorkingArm(uintptr_t medicalSystem, bool* ok) {
    if (s_origHasWorkingArm == nullptr) { *ok = false; return 0; }
    __try {
        uint8_t r = s_origHasWorkingArm(medicalSystem);
        *ok = true;
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ok = false;
        return 0;
    }
}

// Lee la salud REAL (flesh, part+0x40) de los dos brazos del char y decide si hay al menos uno
// funcional. Cadena canónica MedicalSystem (mismos offsets que SEH_WriteLimbHealthDirect/
// SEH_ReassessCollapse): partArray@char+0x5F8, count@char+0x5F0, stride 8, flesh@part+0x40. POD puro.
// Devuelve: 1 = al menos un brazo con flesh>0 (utilizable), 0 = ambos brazos rotos de verdad
//          (flesh<=0), -1 = no legible (cadena de salud incompleta) → el caller delega al original.
// EN: Reads the REAL health (flesh, part+0x40) of both arms and decides whether at least one works.
//     Canonical MedicalSystem chain (same offsets as SEH_WriteLimbHealthDirect/SEH_ReassessCollapse).
//     Returns: 1 = at least one arm with flesh>0 (usable), 0 = both arms really broken (flesh<=0),
//     -1 = unreadable (incomplete health chain) → the caller delegates to the original. Pure POD.
static int SEH_ReadArmRealOk(uintptr_t charPtr) {
    __try {
        const auto& offsets = game::GetOffsets().character;
        if (offsets.healthPartArray < 0 || offsets.healthBase < 0) return -1;
        uintptr_t partArray = 0;
        if (!Memory::Read(charPtr + offsets.healthPartArray, partArray) || !GateIsHeap(partArray))
            return -1;
        int count = 0;
        // El array debe cubrir al menos hasta RightArm (idx 4) → count >= 5. Humanos = 7.
        // EN: The array must cover at least up to RightArm (idx 4) → count >= 5. Humans = 7.
        if (!Memory::Read(charPtr + offsets.healthPartCount, count) ||
            count <= kArmPartIdxRight || count > 32)
            return -1;
        bool anyFunctional = false;
        bool anyReadable   = false;
        const int idx[2] = { kArmPartIdxLeft, kArmPartIdxRight };
        for (int k = 0; k < 2; ++k) {
            uintptr_t part = 0;
            if (!Memory::Read(partArray + idx[k] * offsets.healthStride, part) || !GateIsHeap(part))
                continue;
            float flesh = 0.0f, fleshStun = 0.0f;
            if (!Memory::Read(part + offsets.healthBase, flesh)) continue;
            Memory::Read(part + offsets.healthBase + 4, fleshStun);   // fleshStun = part+0x44
            anyReadable = true;
            // Salud EFECTIVA (flesh - fleshStun), igual criterio que getCollapseStage/reassessCollapseMode
            // (ver SEH_ReassessCollapse) — si solo miráramos flesh>0 anularíamos un colapso por stun de
            // combate legítimo, dejando al host inmune al stun de brazo (mismo bug que ya se evitó en
            // SeedHostCharArmFlagsTick con clearStun=false).
            // EN: EFFECTIVE health (flesh - fleshStun), same criterion as getCollapseStage/reassessCollapseMode (see
            //     SEH_ReassessCollapse) — looking only at flesh>0 would cancel a legitimate combat stun collapse and
            //     make the host immune to arm stun (same bug already avoided in SeedHostCharArmFlagsTick with
            //     clearStun=false).
            if (flesh - fleshStun > 0.0f) anyFunctional = true;
        }
        if (!anyReadable) return -1;                  // ninguna parte de brazo legible → delegar
        return anyFunctional ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ── DETOUR de MedicalSystem::hasWorkingArm (0x644150) ──
// uint8_t __fastcall hasWorkingArm(MedicalSystem* this)
// EN: ── DETOUR of MedicalSystem::hasWorkingArm (0x644150) ── uint8_t __fastcall(MedicalSystem* this)
static uint8_t __fastcall Hook_HasWorkingArm(uintptr_t medicalSystem) {
    // (1) Veredicto nativo SIEMPRE primero (barato: 2 bytes cacheados). Es el valor por defecto.
    // EN: (1) Native verdict ALWAYS first (cheap: 2 cached bytes). It is the default value.
    bool origOk = false;
    uint8_t origResult = SEH_CallHasWorkingArm(medicalSystem, &origOk);

    // (2) Sólo intervenimos para chars de la facción del host/jugador. Cache no listo o puntero
    //   basura → devolver el veredicto nativo sin tocar nada.
    // EN: (2) Only intervene for chars of the host/player faction. Cache not ready or garbage pointer →
    //     return the native verdict untouched.
    uintptr_t hostFaction = s_cachedHostFaction.load(std::memory_order_acquire);
    if (hostFaction == 0 || !GateIsHeap(medicalSystem) || medicalSystem < kMedicalSystemOffset)
        return origResult;

    uintptr_t charPtr = medicalSystem - kMedicalSystemOffset;   // MedicalSystem inline en char+0x458
    if (!GateIsHeap(charPtr)) return origResult;

    uintptr_t charFaction = 0;
    if (!Memory::Read(charPtr + game::GetOffsets().character.faction /*+0x10*/, charFaction) ||
        charFaction != hostFaction) {
        return origResult;   // NPC / char remoto (facción distinta a la del host) → delegar
    }

    // (3) Char del host: evaluar la salud REAL de los brazos (no forzar true a ciegas).
    // EN: (3) Host char: evaluate the REAL arm health (do not force true blindly).
    int realOk = SEH_ReadArmRealOk(charPtr);
    if (realOk == 1 && origResult == 0) {
        // CORRECCIÓN: la salud real dice "brazo funcional" pero el bool nativo cacheado (basura del
        //   clon reclamado) decía "sin brazo" → devolvemos 1. Ataca la causa raíz en el único punto
        //   de choque de los 7 callers GOAP, ANTES de que la acción (cargar/atacar/usar) se descarte.
        // EN: CORRECTION: real health says "working arm" but the cached native bool (clone garbage) said "no arm"
        //     → return 1. Attacks the root cause at the single choke point of the 7 GOAP callers, BEFORE the
        //     action (carry/attack/use) is discarded.
        int c = s_armGateCorrections.fetch_add(1, std::memory_order_relaxed) + 1;
        if (c <= 8 || (c % 200) == 0) {
            spdlog::info("[FIX-ARMGATE] host char=0x{:X} med=0x{:X}: hasWorkingArm nativo=0 pero salud "
                         "real de brazo>0 → CORREGIDO a 1 (desbloquea cargar/atacar/usar) "
                         "[correcciones={}]", charPtr, medicalSystem, c);
        }
        return 1;
    }
    // realOk==0 (ambos brazos rotos de verdad) / realOk==-1 (no legible) / el nativo ya daba 1:
    //   delegar al veredicto nativo — NUNCA forzamos true sobre un brazo genuinamente destrozado.
    // EN: realOk==0 (both arms really broken) / realOk==-1 (unreadable) / native already 1: delegate to the
    //     native verdict — true is NEVER forced on a genuinely destroyed arm.
    return origResult;
}

// Instala el hook de hasWorkingArm (una vez). Se llama desde OnGameTick, junto a los demás gate hooks.
// EN: Installs the hasWorkingArm hook (once). Called from OnGameTick next to the other gate hooks.
static void InstallArmGateHookOnce(uintptr_t modBase) {
    if (s_armGateInstalled.load(std::memory_order_acquire)) return;
    if (modBase == 0) return;
    // RVA confirmada byte a byte (investigación 2026-07-13), AOB único en .text:
    //   80 B9 65 01 00 00 00 75 0C 80 B9 66 01 00 00 00 75 03 32 C0 C3 B0 01 C3
    // Prólogo `cmp byte[rcx+0x165],0` (7 bytes, NO mov rax,rsp) → MinHook normal.
    // EN: RVA confirmed byte by byte (2026-07-13), unique AOB in .text (bytes above). Prologue
    //     `cmp byte[rcx+0x165],0` (7 bytes, NOT mov rax,rsp) → normal MinHook.
    uintptr_t addr = modBase + 0x644150;
    bool ok = HookManager::Get().InstallAt("ArmGate", addr, &Hook_HasWorkingArm, &s_origHasWorkingArm);
    if (ok) {
        s_armGateInstalled.store(true, std::memory_order_release);
        spdlog::info("[FIX-ARMGATE] hook de MedicalSystem::hasWorkingArm INSTALADO en 0x{:X} "
                     "(RVA 0x644150). Los chars del host con brazo sano dejarán de ver el gate "
                     "'brazo roto' por el bool cacheado corrupto del clon.", addr);
    } else {
        spdlog::warn("[FIX-ARMGATE] FALLO al instalar el hook en 0x{:X} (RVA 0x644150) — el host "
                     "podría seguir sin poder cargar/atacar por el flag de brazo corrupto.", addr);
        s_armGateInstalled.store(true, std::memory_order_release); // no reintentar en bucle
    }
}

// ES: Tick principal por frame del mod (hilo de juego: lo llaman el hook TimeUpdate y, de respaldo, el
//     hook Present). Orden: diagnóstico previo → gate de conexión → deduplicación por frame (≥4 ms) →
//     keepalive cada 5 s → gate de partida cargada (con red de seguridad PollForGameLoad) → migas de pan →
//     FIX-FACTORY-RETRY → Step 0 (forzar despausa + DIAGs de simulación/combate + fixes one-shot del host
//     + instalación de hooks de gates) → escaneo de entidades → cola de comandos de red → pipeline de sync
//     (rama nueva SyncOrchestrator o rama legacy de doble buffer) → validación periódica de facciones
//     remotas → diagnóstico y depurador de pipeline. Cada paso actualiza g_lastTickStep/g_lastStepName
//     para que el VEH sepa dónde petó.
// EN: Main per-frame tick of the mod (game thread: called by the TimeUpdate hook and, as fallback, by the
//     Present hook). Order: pre-check diagnostics → connection gate → per-frame dedup (≥4 ms) → keepalive
//     every 5 s → game-loaded gate (with the PollForGameLoad safety net) → breadcrumbs →
//     FIX-FACTORY-RETRY → Step 0 (force unpause + simulation/combat DIAGs + one-shot host fixes + gate hook
//     installs) → entity scan → network command queue → sync pipeline (new SyncOrchestrator branch or
//     legacy double-buffer branch) → periodic remote faction validation → diagnostics and pipeline
//     debugger. Each step updates g_lastTickStep/g_lastStepName so the VEH knows where it crashed.
void Core::OnGameTick(float deltaTime) {
    // ES: ── Diagnóstico previo ── cuenta llamadas y avisa por OutputDebugString las 5 primeras y cada 3000,
    //     incluso estando desconectado.
    // EN: ── Pre-check diagnostics ── counts calls and reports through OutputDebugString the first 5 and
    //     every 3000, even while disconnected.
    // ── Pre-check diagnostics ──
    static int s_preCheckCount = 0;
    s_preCheckCount++;
    if (s_preCheckCount <= 5 || s_preCheckCount % 3000 == 0) {
        char buf[128];
        sprintf_s(buf, "KMP: OnGameTick PRE-CHECK #%d connected=%d\n",
                  s_preCheckCount, m_connected.load() ? 1 : 0);
        OutputDebugStringA(buf);
    }

    // ES: Gate principal: sin conexión no se hace nada más.
    // EN: Main gate: nothing else runs while disconnected.
    if (!m_connected) return;

    // ES: ── Guard de deduplicación por frame ── OnGameTick lo mueven DOS hooks (TimeUpdate y Present) por
    //     redundancia, pero el pipeline (swap de buffers, lanzar trabajo de fondo) NO es idempotente: un doble
    //     swap deshace el primero. Se exige un intervalo mínimo de 4 ms entre ticks completos.
    // ── Per-frame dedup guard ──
    // OnGameTick is driven from BOTH time_hooks (TimeUpdate) and render_hooks (Present)
    // as a redundancy measure. But the pipeline (buffer swap, background work kick)
    // is NOT idempotent — double-swapping reverses the first swap. Use a minimum
    // interval to ensure only one full tick processes per frame (~6ms = 166fps max).
    {
        static auto s_lastTickTime = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        auto sinceLast = std::chrono::duration_cast<std::chrono::microseconds>(now - s_lastTickTime);
        if (sinceLast.count() < 4000) return; // Skip if called within 4ms (same frame)
        s_lastTickTime = now;
    }

    // ES: ── Keepalive ── El servidor desconecta tras 10 s sin tráfico. Se envía C2S_Keepalive cada 5 s.
    //     Va antes del gate de partida cargada para que salga también durante la carga. s_lastKeepalive es
    //     de ámbito de fichero para poder reiniciarlo al reconectar.
    // ── Keepalive ──
    // The server has a 10-second idle timeout. When a player stands still and
    // stops sending position updates, the server disconnects them. Send a
    // keepalive every 5 seconds (well under the timeout) to prevent this.
    // Placed before the game-loaded gate so keepalives fire even during loading.
    // s_lastKeepalive is file-scope so it can be reset on reconnect.
    {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - s_lastKeepalive).count() >= 5) {
            s_lastKeepalive = now;
            PacketWriter writer;
            writer.WriteHeader(MessageType::C2S_Keepalive);
            m_client.SendReliable(writer.Data(), writer.Size());
        }
    }

    // ES: ── Gate de partida cargada ── No correr el pipeline del mundo hasta que termine la carga (si no,
    //     HandleSpawnQueue podría crear un personaje con las texturas aún sin cargar → crash). Respaldo:
    //     detectar la carga porque PlayerBase pase a ser un puntero de heap válido (ruta "conectar antes de
    //     cargar").
    // ── Game-loaded gate ──
    // Don't run game-world pipeline steps until the game has finished loading.
    // entity_hooks triggers OnGameLoaded once the factory is captured + enough creates.
    // Fallback: detect via PlayerBase becoming valid (for connect-before-load path
    // where entity_hooks hasn't captured the factory yet).
    // Without this gate, HandleSpawnQueue can call SpawnCharacterDirect during
    // loading — the factory creates a character before textures are ready → crash.
    if (!m_gameLoaded) {
        if (m_gameFuncs.PlayerBase != 0) {
            uintptr_t playerPtr = 0;
            uintptr_t modBase = m_scanner.GetBase();
            size_t modSize = m_scanner.GetSize();
            if (Memory::Read(m_gameFuncs.PlayerBase, playerPtr) && playerPtr != 0 &&
                playerPtr > 0x10000 && playerPtr < 0x00007FFFFFFFFFFF &&
                !(playerPtr >= modBase && playerPtr < modBase + modSize)) {
                spdlog::info("Core: Game loaded detected via PlayerBase fallback (0x{:X})", playerPtr);
                OnGameLoaded();
            }
        }

        // ⚠ RED DE SEGURIDAD (gameLoaded=false eterno) — flujo "connected-then-load":
        //   El fallback de arriba MUERE en Steam porque el global PlayerBase no se
        //   resuelve (lee bytes de prólogo de función, no un puntero de datos → se
        //   limpia a 0 en InitScanner). Si además la carga no genera un gap >2s
        //   detectable por render_hooks, NADA llama a PollForGameLoad y m_gameLoaded
        //   se queda en false para siempre. Aquí, estando conectados y aún sin cargar,
        //   bombeamos PollForGameLoad ~cada 1s como último recurso: valida el
        //   GameWorldSingleton (que SÍ se resuelve en Steam) y aplica sus timeouts
        //   (90s/120s) para forzar OnGameLoaded si todo lo demás falla.
        //   PollForGameLoad ya gestiona internamente el caso connected (ver su guard).
        // EN: SAFETY NET (gameLoaded=false forever) — "connected-then-load" flow: the fallback above DIES on Steam
        //     because the PlayerBase global does not resolve (it reads function prologue bytes, not a data pointer
        //     → cleared to 0 in InitScanner). If the load also produces no >2 s gap detectable by render_hooks,
        //     NOTHING calls PollForGameLoad and m_gameLoaded stays false forever. Here, while connected and not
        //     loaded, PollForGameLoad is pumped ~every 1 s as a last resort: it validates GameWorldSingleton
        //     (which DOES resolve on Steam) and applies its timeouts to force OnGameLoaded if all else fails.
        if (m_connected.load(std::memory_order_acquire)) {
            static int s_loadPollCounter = 0;
            if (++s_loadPollCounter % 150 == 0) { // ~1s a 150 fps
                PollForGameLoad();
            }
        }
        return;
    }

    // ES: ── Seguimiento de pasos: contadores que lee el VEH para saber en qué paso petó ──
    // ── Step tracking: member variable so SEH crash handler can report which step crashed ──
    static int s_tickCallCount = 0;
    s_tickCallCount++;
    g_tickNumber = s_tickCallCount;
    g_lastTickStep = 0;
    g_lastStepName = "tick_entry";

    // ES: Registrar las primeras entradas para confirmar que OnGameTick está corriendo.
    // Log first few successful entries to confirm OnGameTick is running
    if (s_tickCallCount <= 5 || s_tickCallCount % 200 == 0) {
        char buf[128];
        sprintf_s(buf, "KMP: OnGameTick ENTERED #%d (dt=%.4f)\n", s_tickCallCount, deltaTime);
        OutputDebugStringA(buf);
        spdlog::info("Core::OnGameTick ENTERED (call #{}, dt={:.4f}, lastStep={})",
                     s_tickCallCount, deltaTime, m_lastCompletedStep.load());
    }
    WriteBreadcrumb("tick_entry", s_tickCallCount);
    SetLastCompletedStep(0);

    // ── FIX-FACTORY-RETRY (2026-06-20): reintento PERSISTENTE de la captura del factory ──
    // CAUSA RAÍZ (verificada por RE de bytes, Steam 1.0.68):
    //   theFactory @ RVA 0x21345B0 es un PUNTERO GLOBAL que el ctor de GameWorld (0x874E70)
    //   pone a NULL en 0x874EF2 (`mov [GW+0x4A0], r13` con r13=0). El juego lo rellena LAZY,
    //   solo cuando ejecuta RootObjectFactory::create (0x583400) -> process (0x581770) por
    //   PRIMERA vez. Verificado: 98 lecturas `mov rcx,[0x21345B0]`, 0 escrituras y 0 LEA
    //   estáticos en .text -> el slot se inicializa en runtime, no antes.
    //
    //   En el flujo host = "Multiplayer NEW GAME", cuando CaptureFactoryFromGameWorld corría
    //   UNA sola vez en OnGameLoaded, el slot AÚN estaba NULL/residuo (0x590301A0) porque el
    //   primer create del personaje del host todavía no había ocurrido -> m_factory=0 para
    //   siempre. El único retry existente (HandleSpawnQueue ~8090) está GATEADO por
    //   `queueSize>0`: sin joiners no hay spawns pendientes -> NUNCA se reintenta.
    //
    // FIX: reintentar la captura cada ~1s mientras el juego esté cargado y aún sin factory.
    //   En cuanto el host cree su primer personaje (y el juego llene 0x21345B0), la siguiente
    //   pasada captura el factory real y IsReady() pasa a true SIN forzar creates ni spawnar
    //   NPCs dummy. CaptureFactoryFromGameWorld ya es SEH-safe (valida con Memory::Read y
    //   el check OBLIGATORIO de vtable en .rdata + entradas en .text — los checks #4/#4b de
    //   "mitad alta nula" se ELIMINARON en Oleada A: el heap de Kenshi vive bajo 4GB y
    //   rechazaban el factory real). Coste: 1 comparación + (cuando falta) una
    //   lectura de puntero, una vez por segundo. No-op en cuanto IsReady()==true.
    // EN: ── FIX-FACTORY-RETRY (2026-06-20): PERSISTENT retry of the factory capture ──
    //     ROOT CAUSE (byte RE, Steam 1.0.68): theFactory @ RVA 0x21345B0 is a GLOBAL POINTER that the GameWorld
    //       ctor (0x874E70) sets to NULL at 0x874EF2 (`mov [GW+0x4A0], r13` with r13=0). The game fills it
    //       LAZILY, only when RootObjectFactory::create (0x583400) -> process (0x581770) runs for the FIRST
    //       time (98 reads `mov rcx,[0x21345B0]`, 0 static writes → runtime init).
    //       In the host "Multiplayer NEW GAME" flow, when CaptureFactoryFromGameWorld ran ONCE in OnGameLoaded
    //       the slot was STILL NULL/stale because the host first character create had not happened →
    //       m_factory=0 forever. The only existing retry (in HandleSpawnQueue) is GATED by queueSize>0: with no
    //       joiners there are no pending spawns → NEVER retried.
    //     FIX: retry the capture every ~1 s while the game is loaded and there is still no factory. As soon as
    //       the host creates its first character (and the game fills 0x21345B0), the next pass captures the
    //       real factory and IsReady() becomes true WITHOUT forcing creates. CaptureFactoryFromGameWorld is
    //       SEH-safe (Memory::Read + MANDATORY vtable-in-.rdata check + entries in .text; the "high half null"
    //       checks #4/#4b were REMOVED in wave A because the Kenshi heap lives below 4 GB and they rejected the
    //       real factory). Cost: one compare + (while missing) one pointer read per second.
    if (!m_spawnManager.IsReady() && m_gameFuncs.GameWorldSingleton != 0) {
        static int s_factoryRetryGuard = 0;
        if (++s_factoryRetryGuard % 150 == 0) { // ~1s a 150 fps
            uintptr_t sb = m_scanner.GetBase();
            size_t    ss = m_scanner.GetSize();
            if (ValidateGameWorldGlobal(m_gameFuncs.GameWorldSingleton, sb, ss)) {
                if (m_spawnManager.CaptureFactoryFromGameWorld(m_gameFuncs.GameWorldSingleton)) {
                    spdlog::info("Core: [FIX-FACTORY-RETRY] factory capturado en reintento por tick "
                                 "(IsReady()=true) — el slot global 0x21345B0 quedó poblado tras el "
                                 "primer create del host");
                    m_nativeHud.LogStep("OK", "Factory capturado (retry por tick)");
                }
            }
        }
    }

    // ── Step 0: Force unpause ──
    // Kenshi allows pausing (Space key) which freezes the game world. In multiplayer
    // this must be prevented — the server keeps ticking regardless. Every tick, force
    // GameWorld.paused = false and write server game speed to prevent local overrides.
    //
    // ⚠ BUG HISTÓRICO ARREGLADO (causa raíz del "combate no funciona"):
    //   El código anterior hacía `gwPtr = *gwSingleton` (dereferencia ciega) y escribía
    //   en `gwPtr + 0x8B9`. En Steam 1.0.68 GameWorld es una INSTANCIA EMBEBIDA en .data,
    //   así que `*gwSingleton` devuelve la VTABLE (en .text), y `vtable+0x8B9` cae en una
    //   página .text/.rdata de SOLO LECTURA. La escritura de paused=0 fallaba en silencio
    //   → el cliente quedaba PAUSADO → la simulación (IA/combate/movimiento) nunca avanzaba
    //   aunque el servidor enviara speed=1. Ahora usamos GameWorldAccessor, que resuelve
    //   el objeto real vía ResolveWorldObject() (maneja instancia-embebida y puntero).
    // EN: ── Step 0: Force unpause ── (see the English text above).
    //     HISTORICAL BUG FIXED (root cause of "combat does not work"): the old code did `gwPtr = *gwSingleton`
    //     (blind deref) and wrote at `gwPtr + 0x8B9`. In Steam 1.0.68 GameWorld is an EMBEDDED INSTANCE in
    //     .data, so `*gwSingleton` returns the VTABLE (in .text) and `vtable+0x8B9` lands on a READ-ONLY page.
    //     The paused=0 write failed silently → the client stayed PAUSED → simulation (AI/combat/movement) never
    //     advanced even though the server sent speed=1. Now GameWorldAccessor resolves the real object through
    //     ResolveWorldObject() (handles embedded instance and pointer).
    // ES: ── Step 0: forzar despausa ── Kenshi permite pausar (Espacio) congelando el mundo; en multijugador
    //     hay que impedirlo porque el servidor sigue avanzando: cada tick se fuerza GameWorld.paused = false
    //     (GW+0x8B9) y la velocidad del servidor (GW+0x700).
    {
        game::GameWorldAccessor world(game::GetResolvedGameWorld());
        if (world.IsValid()) {
            // ── [DIAG] Volcado del valor REAL del flag paused antes de actuar ──
            // -1 = no resuelto/no legible, 0 = despausado, 1 = pausado.
            // EN: ── [DIAG] Dump of the REAL paused flag value before acting ── -1 = unresolved/unreadable,
            //     0 = unpaused, 1 = paused.
            int   pausedRaw = world.GetPausedRaw();
            float speedRaw  = world.GetGameSpeed();

            // ── [DIAG-PAUSE2] Cache de pausa de subsistema (obj+0xB8) ──
            // RE confirmó que el setter oficial cachea isPaused en subsistema+0xB8.
            // Si la sim está despausada (pausedRaw=0) pero este cache sigue en 1, ESA es
            // la "pausa fantasma" que bloquea las órdenes. Resolvemos el subsistema como
            // [GameWorld+0x18] (uno de los 3 que toca el listener 0x78AC50) y leemos +0xB8.
            // EN: ── [DIAG-PAUSE2] Subsystem pause cache (obj+0xB8) ── RE confirmed that the official setter caches
            //     isPaused at subsystem+0xB8. If the sim is unpaused (pausedRaw=0) but this cache is still 1, THAT is
            //     the "ghost pause" blocking orders. The subsystem is resolved as [GameWorld+0x18] (one of the 3 the
            //     listener 0x78AC50 touches) and +0xB8 is read.
            int subsysPausedCache = -1; // -1 = no legible
            {
                uintptr_t gwObj = world.GetWorldObject();
                if (gwObj != 0) {
                    uintptr_t subsys = 0;
                    if (Memory::Read(gwObj + 0x18, subsys) && subsys != 0) {
                        uint8_t cache = 0;
                        if (Memory::Read(subsys + 0xB8, cache))
                            subsysPausedCache = (cache != 0) ? 1 : 0;
                    }
                }
            }

            static int s_pauseDiagCount = 0;
            if (++s_pauseDiagCount <= 10 || s_pauseDiagCount % 600 == 0) {
                spdlog::info("[DIAG-PAUSE] paused(+0x8B9)={} gameSpeed(+0x700)={:.2f} "
                             "subsysCache([gw+0x18]+0xB8)={} officialSetter={} worldObj=0x{:X}",
                             pausedRaw, speedRaw, subsysPausedCache,
                             game::HasGameSetPausedFn() ? "yes" : "NO",
                             world.GetWorldObject());
            }

            // ── [DIAG-SIM] Gate de pausa de mainLoop + round-robin del AI tick ──────
            // CAUSA RAÍZ confirmada por RE de bytes (Fase 4, doble verificación 2026-06-18):
            //
            //   GameWorld::mainLoop (0x788A00), justo antes de llamar a updateCharacters,
            //   hace en 0x788FF5:  cmp byte[GW+0x8B9],0 ; jne <rama-paused>.
            //   • Si GW+0x8B9 == 0 → llama updateCharacters (0x786E30) → bucle 1 ejecuta
            //     el AI tick [vtbl+0xE8] (combate, Jobs, levantarse de cama, recuperar KO,
            //     regen de stun) para los chars seleccionados ese frame.
            //   • Si GW+0x8B9 != 0 → SALTA updateCharacters y ejecuta la rama "paused"
            //     (0x787230) que solo llama [vtbl+0x270] (tick reducido: posición/anim).
            //   → Con el gate pegado en 1, el RELOJ avanza y los personajes se mueven/animan
            //     pero NO atacan ni se recuperan. Es EXACTAMENTE el síntoma observado.
            //
            //   El round-robin del AI tick (contadores .data 0x2132EC8/ECC/ED0/ED4) es
            //   AUTO-RECUPERANTE: el cursor 0x2132ED4 se resetea a 0 cada frame (0x786EAA)
            //   y con pocos chars (host) TODOS reciben tick. NO puede congelarse a 0 → NO es
            //   la causa. El contador clave del AI tick de combate es 0x2132ED0 (clase A).
            //
            //   GW+0x8B8 es solo un flag "updateCharacters en curso" (=1 entra, =0 sale) que
            //   NO se lee en ningún sitio → INOCUO, NO es guard funcional. Pista MUERTA.
            //
            // Por qué el gate se queda pegado: el setter oficial 0x787D40 escribe
            //   GW+0x8B9 = argBool OR (gameSpeed[GW+0x700] == 0.0f exacto).
            // Si el host pausó (gameSpeed==0.0), aunque llamemos SetPaused(false) el OR lo
            // re-pega a 1. Por eso el FIX-SIM de abajo restaura gameSpeed ANTES de despausar.
            //
            // Este DIAG vuelca SOLO LECTURA (SEH): el gate +0x8B9, gameSpeed, el contador de
            // AI-tick clase A (0x2132ED0) y el cursor (0x2132ED4) para confirmar en vivo cuál
            // de los dos mecanismos está pasando (gate pegado vs round-robin congelado).
            // EN: ── [DIAG-SIM] mainLoop pause gate + AI tick round-robin ── ROOT CAUSE confirmed by byte RE (Phase 4):
            //     GameWorld::mainLoop (0x788A00), right before updateCharacters, does at 0x788FF5: cmp byte[GW+0x8B9],0;
            //     jne <paused-branch>. GW+0x8B9 == 0 → updateCharacters (0x786E30) runs the AI tick [vtbl+0xE8]
            //     (combat, Jobs, get up from bed, recover from KO, stun regen). GW+0x8B9 != 0 → skips it and runs the
            //     "paused" branch (0x787230, only [vtbl+0x270]: position/anim). → With the gate stuck at 1 the CLOCK
            //     advances and characters move/animate but do NOT attack nor recover — EXACTLY the observed symptom.
            //     The AI tick round-robin (.data counters 0x2132EC8/ECC/ED0/ED4) is SELF-RECOVERING (cursor reset every
            //     frame) → NOT the cause. GW+0x8B8 is just an "updateCharacters in progress" flag read nowhere → dead
            //     lead. Why the gate sticks: the official setter 0x787D40 writes GW+0x8B9 = argBool OR
            //     (gameSpeed[GW+0x700] == 0.0f); if the host paused (gameSpeed==0.0) SetPaused(false) re-sets it → the
            //     FIX-SIM below restores gameSpeed BEFORE unpausing. This DIAG is READ ONLY (SEH).
            {
                static int s_simDiagCount = 0;
                static uint32_t s_prevFrameCtr = 0xFFFFFFFF;
                static uint32_t s_prevCtrC = 0xFFFFFFFF;
                static double   s_prevSimClock = -2.0;   // reloj de sim del frame anterior
                if (++s_simDiagCount <= 20 || s_simDiagCount % 120 == 0) {
                    uintptr_t modBase     = Memory::GetModuleBase();
                    uintptr_t gwObj       = world.GetWorldObject();
                    void*     primaryChar = m_playerController.GetPrimaryCharacter();

                    SimDiagSnapshot snap;
                    SEH_ReadSimDiag(modBase, gwObj, primaryChar, &snap);

                    // ¿Avanza el cursor de frame y el contador de AI tick clase A?
                    // EN: Are the frame cursor and the class-A AI tick counter advancing?
                    const char* frameAdv = (s_prevFrameCtr == 0xFFFFFFFF) ? "?"
                                         : (snap.frameCtr != s_prevFrameCtr) ? "YES" : "FROZEN";
                    const char* ctrCAdv  = (s_prevCtrC == 0xFFFFFFFF) ? "?"
                                         : (snap.aiTickClassA != s_prevCtrC) ? "YES" : "FROZEN";
                    s_prevFrameCtr = snap.frameCtr;
                    s_prevCtrC     = snap.aiTickClassA;

                    spdlog::info("[DIAG-SIM] GATE pause(+0x8B9)={} gameSpeed(+0x700)={:.3f} "
                                 "==> {} | aiTickClassA(0x2132ED0)={} adv={} "
                                 "frameCursor(0x2132ED4)={} adv={} otherCtrs=[{},{}] "
                                 "activeChars(+0x770)={} primaryChar=0x{:X} primaryLOD(+0xE4)={}",
                                 snap.gatePause, snap.gameSpeed,
                                 (snap.gatePause == 0) ? "AI-TICK CORRE" : "PAUSED-SKIP (combate congelado)",
                                 snap.aiTickClassA, ctrCAdv,
                                 snap.frameCtr, frameAdv,
                                 snap.ctrA, snap.ctrB,
                                 (snap.activeCnt == (uint64_t)-1) ? -1 : (long long)snap.activeCnt,
                                 reinterpret_cast<uintptr_t>(primaryChar), snap.primaryLod);

                    // ── [DIAG-CLOCK] Reloj de simulación: ¿avanza o congelado? ──────────
                    // ESTA es la prueba decisiva de Fase 4 (pista de doble verificación de
                    // bytes). El AI tick (0x5CCE36) lee SimClock+0xA0 (reloj en horas de
                    // juego = día*24 + horaDelDía) y lo compara con char+0xD0. Si el reloj
                    // NO avanza entre dos muestras (FROZEN) → diff≈0 → el AI tick no procesa
                    // combate/levantarse/recuperar-KO aunque corra (gate+0x8B9==0). Si el
                    // reloj SÍ avanza pero el combate sigue congelado → la causa NO es el
                    // reloj (mirar gate/round-robin/orden). Discriminamos en runtime:
                    //   • simClock FROZEN + gate==0  → CAUSA = reloj de simulación parado.
                    //   • simClock YES   + gate==0   → reloj OK; la congelación es de otra capa.
                    // EN: ── [DIAG-CLOCK] Simulation clock: advancing or frozen? ── The AI tick (0x5CCE36) reads SimClock+0xA0
                    //     (game hours = day*24 + hourOfDay) and compares it with char+0xD0. clock FROZEN + gate==0 → cause =
                    //     stopped simulation clock; clock YES + gate==0 → clock OK, the freeze is in another layer.
                    const char* clockAdv = (s_prevSimClock < -1.5) ? "?"
                                         : (snap.simClock != s_prevSimClock) ? "YES" : "FROZEN";
                    double clockDelta = (s_prevSimClock < -1.5) ? 0.0 : (snap.simClock - s_prevSimClock);
                    s_prevSimClock = snap.simClock;

                    spdlog::info("[DIAG-CLOCK] simClock(*(0x21303D0)+0xA0)={:.6f} adv={} "
                                 "delta={:.6f} | simDay(+0x08)={} simTimeOfDay([+0x20]+0x1C)={:.5f} "
                                 "| char+0xD0(lastProcessed)={:.6f} diff(clock-char)={:.6f} "
                                 "| simClockPtr=0x{:X} ==> {}",
                                 snap.simClock, clockAdv, clockDelta,
                                 snap.simDay, snap.simTimeOfDay,
                                 snap.primaryLastT,
                                 (snap.primaryLastT < -0.5 || snap.simClock < -0.5)
                                     ? 0.0 : (snap.simClock - snap.primaryLastT),
                                 snap.simClockPtr,
                                 (snap.simClockPtr == 0) ? "SIN-RELOJ (ptr no resuelto)"
                                   : (clockAdv[0] == 'F') ? "RELOJ CONGELADO (causa Fase 4 confirmada)"
                                   : (clockAdv[0] == 'Y') ? "RELOJ AVANZA (causa NO es el reloj)"
                                   : "primera-muestra");

                    // ── [DIAG-AICHK] Gates del AI tick 0x5CCD90 ────────────────────────
                    // CORREGIDO 2026-06-18 (triple verificación RE): +0x5BC = flag MUERTO
                    // (0=VIVO, 1=MUERTO). Interpretación real del gate 0x5CCE24/2B:
                    //   • +0x5BC == 0 (VIVO) → SALTA a 0x5CD1C0 = RAMA VIVA (IA/combate vía
                    //     vtable; gate interno char+0xD8 vs 0.75 decide el "think" pesado).
                    //     El char del host DEBE estar aquí: si está vivo y no pelea, la causa
                    //     NO es +0x5BC ni el seed de +0xD0 — es aguas arriba (no entra en
                    //     updateCharacters / gate GW+0x8B9).
                    //   • +0x5BC != 0 (MUERTO) → rama CADÁVER: catch-up con +0xD0/+0x3D4/+0x2F8
                    //     y umbral 12.0. SOLO aquí aplica el seed de +0xD0 (es decay de cuerpo).
                    // Throttle idéntico al de DIAG-CLOCK (mismo bloque s_simDiagCount). SOLO lectura.
                    // EN: ── [DIAG-AICHK] AI tick 0x5CCD90 gates ── CORRECTED 2026-06-18: +0x5BC = DEAD flag (0=ALIVE,
                    //     1=DEAD). +0x5BC == 0 (ALIVE) → jumps to 0x5CD1C0 = LIVE BRANCH (AI/combat via vtable; the host char
                    //     MUST be here: if alive and not fighting, the cause is upstream). +0x5BC != 0 (DEAD) → CORPSE branch
                    //     (catch-up with +0xD0/+0x3D4/+0x2F8 and 12.0 threshold; the +0xD0 seed only applies here). READ ONLY.
                    const char* aiVerdict;
                    if (!snap.aiGateRead) {
                        aiVerdict = "gates NO legibles (primaryChar null / AV)";
                    } else if (snap.aiGate5BC == 0) {
                        aiVerdict = "VIVO -> rama 0x5CD1C0 (IA viva OK; si no pelea, causa aguas arriba)";
                    } else {
                        aiVerdict = "MUERTO (+0x5BC!=0) -> rama cadaver/catch-up (seed +0xD0 aplica aqui)";
                    }
                    // Cast a int: uint8_t con spdlog/fmt se imprimiría como carácter, no número.
                    // EN: Cast to int: a uint8_t through spdlog/fmt would print as a character, not a number.
                    spdlog::info("[DIAG-AICHK] +0x5BC={} +0x3D4={} +0x2F8={} (read={}) ==> {}",
                                 static_cast<int>(snap.aiGate5BC),
                                 static_cast<int>(snap.aiGate3D4),
                                 snap.aiGate2F8,
                                 snap.aiGateRead, aiVerdict);

                    // ── [DIAG-THINK] Gate interno de la rama viva 0x5CD1C0 (SEMÁNTICA CORREGIDA) ──
                    // CORRECCIÓN 2026-06-18 (RE de bytes, audit-08-rama-viva-think.md):
                    // char+0xD8 es una CACHÉ derivada de la HORA del juego (reloj global), NO un
                    // acumulador de dt. El gate real en 0x5CD1F4 es `comiss 0.75,[+0xD8]; jbe salir`:
                    //   • +0xD8 >= 0.75  → SALE sin pensar (jbe tomado).      (antes mal etiquetado)
                    //   • +0xD8 <  0.75  → entra al think pesado (think+commit a GameWorld).
                    // ADEMÁS el gate se SALTA si [vtbl+0x58](char)→[ret+0x250] != 0 (jne 0x5CD1FD):
                    // en ese caso piensa SIEMPRE, ignorando +0xD8. Y el flag char+0xDC: si ==0 sale
                    // antes del gate (0x5CD1D4), pero la propia rama viva lo pone a 0 cada vez que
                    // piensa (0x5CD1FD), así que su valor instantáneo NO indica "atascado".
                    // ==> +0xD8/+0xDC NO bloquean el combate del host. Este DIAG solo deja constancia
                    //     de su valor; el veredicto operativo real lo da [DIAG-SIMLIST] + el gate
                    //     GW+0x8B9 / que 0x5CCD90 se invoque. NO derivar "no piensa" solo de +0xD8.
                    // EN: ── [DIAG-THINK] Inner gate of the live branch 0x5CD1C0 (CORRECTED SEMANTICS) ── CORRECTION
                    //     2026-06-18 (audit-08-rama-viva-think.md): char+0xD8 is a CACHE derived from the game HOUR, NOT a dt
                    //     accumulator. Real gate at 0x5CD1F4 `comiss 0.75,[+0xD8]; jbe exit`: +0xD8 >= 0.75 → exits without
                    //     thinking; +0xD8 < 0.75 → heavy think. The gate is SKIPPED if [vtbl+0x58](char)→[ret+0x250] != 0 (it
                    //     always thinks). char+0xDC is reset to 0 by the live branch itself each think, so its instant value
                    //     does not mean "stuck". ==> +0xD8/+0xDC do NOT block host combat; do not derive "does not think" from
                    //     +0xD8 alone.
                    if (snap.aiGate5BC == 0) {
                        const char* thinkVerdict =
                            !snap.thinkRead             ? "no legible" :
                            (snap.thinkTimerD8 < 0.75f) ? "+0xD8<0.75 -> el gate horario PERMITE pensar"
                                                        : "+0xD8>=0.75 -> gate horario salta (salvo [vtbl+0x58]+0x250!=0, que piensa igual)";
                        spdlog::info("[DIAG-THINK] char+0xD8(cache horaria)={:.5f} vs 0.75 | char+0xDC(flag)={} "
                                     "(read={}) ==> {} [NOTA: +0xD8 NO es la causa del combate congelado]",
                                     snap.thinkTimerD8, static_cast<int>(snap.thinkFlagDC),
                                     snap.thinkRead, thinkVerdict);
                    }

                    // ── [DIAG-SIMLIST] ¿Está el host EN la lista de simulación? (H1) ────
                    // PRUEBA DISCRIMINANTE de la hipótesis H1 (audit-06 §6, la MÁS probable):
                    // updateCharacters itera la lista GW+0x768/+0x770/+0x788 (derivada del
                    // hash set GW+0x750), NO el squad del mod (GW+0x580→+0x2B0). Si el char
                    // primario del host NO está en esa lista, NUNCA recibe el AI tick
                    // [vtbl+0xE8] aunque el reloj avance y el squad sea correcto.
                    //   • hostInSimList == NO  + gate==0 + reloj YES → H1 CONFIRMADA
                    //     (el host está fuera de la simulación; el fix sería insertarlo en
                    //      GW+0x750 vía el insertador del motor 0x5429A0 / cola +0x890, que
                    //      NO se aplica aquí: esto es SOLO lectura).
                    //   • hostInSimList == YES + combate congelado → refuta H1 → mirar H3/H4.
                    // Recorremos la lista IGUAL que el motor, todo bajo SEH (SEH_WalkSimList),
                    // sin escribir nada. lastProcessed(+0xD0) se muestrea para H4 (ya en snap).
                    // EN: ── [DIAG-SIMLIST] Is the host IN the simulation list? (H1) ── updateCharacters iterates the list
                    //     GW+0x768/+0x770/+0x788 (derived from the hash set GW+0x750), NOT the mod squad (GW+0x580→+0x2B0). If
                    //     the host primary char is NOT in it, it NEVER gets the AI tick. hostInSimList == NO + gate==0 + clock
                    //     YES → H1 CONFIRMED (the fix would insert it into GW+0x750 via the engine inserter 0x5429A0 / queue
                    //     +0x890 — NOT applied here: read only). hostInSimList == YES + frozen combat → refutes H1.
                    SEH_WalkSimList(gwObj, primaryChar, &snap);

                    const char* hostInList = (snap.hostInSimList == 1) ? "YES"
                                           : (snap.hostInSimList == 0) ? "NO"
                                           : "UNKNOWN";
                    spdlog::info("[DIAG-SIMLIST] hostInSimList={} simListCount(+0x770)={} "
                                 "nodesWalked={} primaryChar=0x{:X} lastProcessed(+0xD0)={:.6f} "
                                 "==> {}",
                                 hostInList,
                                 (snap.simListCount == (uint64_t)-1) ? -1 : (long long)snap.simListCount,
                                 snap.simListWalked,
                                 reinterpret_cast<uintptr_t>(primaryChar),
                                 snap.primaryLastT,
                                 (snap.hostInSimList == 0) ? "H1 CONFIRMADA (host FUERA de la lista de sim -> sin AI tick)"
                                   : (snap.hostInSimList == 1) ? "H1 REFUTADA (host EN la lista; mirar H3/H4)"
                                   : "INDETERMINADO (lista no recorrible / primaryChar null)");

                    // ── [DIAG-PRIMARY] ¿El mod resuelve el char CORRECTO del jugador? ──────
                    // PRUEBA DISCRIMINANTE de la pista NO refutada (audit-08 Hipótesis A): el mod
                    // agarra data[0] del lektor PlayerInterface+0x2B0, pero el MOTOR usa el char
                    // controlado en PlayerInterface+0x2A8 (RE confirmado por bytes 2026-06-18,
                    // SetControlledChar @0x802520 / consumo @0x50E9CF). Si difieren, los fixes del
                    // mod (faction +0x10, seed +0xD0) caen en el char EQUIVOCADO y el char real del
                    // jugador nunca se toca. Este DIAG vuelca AMBOS (+ la fuente Faction[count-1])
                    // para compararlos en runtime. SOLO LECTURA (SEH), throttle de s_simDiagCount.
                    // EN: ── [DIAG-PRIMARY] Does the mod resolve the player RIGHT char? ── The mod takes data[0] of the
                    //     PlayerInterface+0x2B0 lektor, but the ENGINE uses the controlled char at PlayerInterface+0x2A8
                    //     (SetControlledChar @0x802520 / consumed @0x50E9CF). If they differ, the mod fixes land on the WRONG
                    //     char. Dumps BOTH (+ the Faction[count-1] source). READ ONLY (SEH), s_simDiagCount throttle.
                    {
                        size_t modSizeP = 0x4000000;  // fallback 64MB; afinamos por PE header
                        // EN: Module size: 64 MB fallback, refined from the PE header.
                        {
                            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
                            if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                                auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
                                    modBase + dos->e_lfanew);
                                if (nt->Signature == IMAGE_NT_SIGNATURE)
                                    modSizeP = nt->OptionalHeader.SizeOfImage;
                            }
                        }
                        PrimaryDiagSnapshot psnap;
                        SEH_ReadPrimaryDiag(modBase, modSizeP, gwObj, &psnap);

                        if (!psnap.resolved) {
                            spdlog::info("[DIAG-PRIMARY] PlayerInterface NO resuelto "
                                         "(GW+0x580 null/inválido) — sin datos este frame");
                        } else {
                            // Comparación EXTRA: ¿el char que usa el mod en runtime
                            // (m_playerController.GetPrimaryCharacter(), vía EntityRegistry) coincide
                            // con data[0] del motor y con el controlledChar del motor?
                            // EN: EXTRA comparison: does the char the mod uses at runtime (m_playerController.GetPrimaryCharacter(),
                            //     through EntityRegistry) match the engine data[0] and the engine controlledChar?
                            uintptr_t modUsed = reinterpret_cast<uintptr_t>(primaryChar);
                            const char* usedVsData0 =
                                (modUsed == 0 || psnap.modChar == 0) ? "?"
                                : (modUsed == psnap.modChar) ? "IGUAL" : "DISTINTO";
                            const char* usedVsCtrl =
                                (modUsed == 0 || psnap.ctrlChar == 0) ? "?"
                                : (modUsed == psnap.ctrlChar) ? "IGUAL" : "DISTINTO";

                            spdlog::info(
                                "[DIAG-PRIMARY] PI=0x{:X} | MOD-usa=0x{:X} (vs data0={} vs ctrl={}) "
                                "| data[0](PI+0x2B0)=0x{:X} name='{}' fac=0x{:X} "
                                "lektor[size={} cap={}] | ctrlChar(PI+0x2A8)=0x{:X} name='{}' fac=0x{:X} "
                                "| facMemberCnt(+0x210)={} facLast(+0x218[cnt-1])=0x{:X}",
                                psnap.playerIface, modUsed, usedVsData0, usedVsCtrl,
                                psnap.modChar, psnap.modName, psnap.modFaction,
                                psnap.lektorSize, psnap.lektorCap,
                                psnap.ctrlChar, psnap.ctrlName, psnap.ctrlFaction,
                                psnap.facMemberCnt, psnap.facLastChar);

                            // Veredicto: ¿data[0] (lo que usa el mod) == controlledChar (motor)?
                            // EN: Verdict: data[0] (what the mod uses) == controlledChar (engine)?
                            const char* verdict;
                            if (psnap.ctrlChar == 0) {
                                verdict = "ctrlChar(PI+0x2A8)=0 -> aun sin char controlado "
                                          "(menu/carga) — reintentar";
                            } else if (psnap.modEqCtrl == 1) {
                                verdict = "OK: data[0]==controlledChar -> el mod agarra el char "
                                          "CORRECTO (la causa del combate NO es resolucion de char)";
                            } else if (psnap.modEqCtrl == 0) {
                                verdict = "*** CHAR EQUIVOCADO: data[0] != controlledChar -> el mod "
                                          "aplica fixes (faction/seed) al char QUE NO controla el "
                                          "jugador. FIX: resolver PI+0x2A8 en vez de +0x2B0 data[0] ***";
                            } else {
                                verdict = "no comparable (algun char null este frame)";
                            }
                            spdlog::info("[DIAG-PRIMARY] modEqCtrl={} modEqFacLast={} ctrlEqFacLast={} "
                                         "==> {}",
                                         psnap.modEqCtrl, psnap.modEqFacLast, psnap.ctrlEqFacLast,
                                         verdict);
                        }
                    }

                    // ── [DIAG-AITASK] ¿El char del host tiene IA/jobs, o está "muerto en vida"? ──
                    // ⚠ OFFSET CORREGIDO (2026-06-19): el AITaskSytem NO está en char+0x20 (FALSO:
                    // char+0x18 es el name SSO; char+0x20 caía dentro del buffer del nombre → daba un
                    // FALSO "vtblOk=0" que nos cegaba). La cadena REAL es char+0x650 (AI) -> AI+0x20
                    // (AITaskSytem). CONFIRMADO en attackTarget: mov rcx,[char+0x650]; mov rcx,[rcx+0x20].
                    //
                    // DIAG AMPLIADO (host vs NPC sano lado a lado) para las 3 sondas decisivas:
                    //   (a) AI+0x10 = Platoon registrado. 0/basura => Tasker HUÉRFANO => el AI tick no
                    //       consume NINGÚN Task (atacar / levantarse de cama / curar) => amIdle=1 SIEMPRE.
                    //       Es la CAUSA TRANSVERSAL. Predicción: NPC AI+0x10!=0; host AI+0x10=0/basura.
                    //       Lo repara [FIX-PLATOON] (setActivePlatoon 0x6213F0 -> 0x506CC0: mov [AI+0x10],me).
                    //   (b) CombatClass = *(char+0x648 [CharBody] +0x8). 0 => falla el ATAQUE concreto.
                    //       Predicción: NPC !=0; host=0. Lo repara [FIX-COMBATCLASS] (CharBody::create).
                    //   (c) Tasker individual = *(char+0x448 [AnimationClass] +0xE8) -> vt[+0x10] (runAction):
                    //       lo que el AI tick (0x5CCD90) ejecuta. Se vuelca para descartar este eslabón.
                    // Offsets CONFIRMADOS por RE de bytes (Steam 1.0.68). SOLO LECTURA.
                    // EN: ── [DIAG-AITASK] Does the host char have AI/jobs, or is it "dead while alive"? ── CORRECTED OFFSET
                    //     (2026-06-19): the AITaskSytem is NOT at char+0x20 (inside the SSO name buffer); the real chain is
                    //     char+0x650 (AI) -> AI+0x20 (AITaskSytem). EXTENDED DIAG (host vs healthy NPC side by side) for the 3
                    //     decisive probes: (a) AI+0x10 = registered Platoon (0/garbage → ORPHANED Tasker → amIdle=1 ALWAYS,
                    //     the CROSS-CUTTING CAUSE, fixed by [FIX-PLATOON]); (b) CombatClass = *(char+0x648+0x8) (0 → the
                    //     concrete ATTACK fails, fixed by [FIX-COMBATCLASS]); (c) individual Tasker = *(char+0x448+0xE8) ->
                    //     vt[+0x10] (runAction). READ ONLY.
                    {
                        AITaskSnapshot ai;
                        SEH_ReadAITaskDiag(modBase, gwObj, primaryChar, &ai);

                        // isHeap local (mismo criterio del mod) para clasificar AI+0x10/CombatClass como
                        // "ausente/basura" vs "presente" en el veredicto (no toca memoria del juego).
                        // EN: Local isHeap (mod criterion) to classify AI+0x10/CombatClass as missing/garbage vs present in the
                        //     verdict (does not touch game memory).
                        auto isHeapV = [](uintptr_t v) -> bool {
                            return v >= 0x10000 && v < 0x00007FFFFFFFFFFF && (v & 0x7) == 0;
                        };
                        // Banderas legibles del estado del host (1=presente/válido, 0=ausente/basura).
                        // EN: Readable flags of the host state (1=present/valid, 0=missing/garbage).
                        const bool hostHasAI      = isHeapV(ai.hostAI);
                        const bool hostHasPlatoon = isHeapV(ai.hostPlatoon);   // AI+0x10 registrado
                        const bool hostHasCombat  = isHeapV(ai.hostCombat);    // *(CharBody+0x8)
                        const bool npcHasPlatoon  = ai.npcFound && isHeapV(ai.npcPlatoon);
                        const bool npcHasCombat   = ai.npcFound && isHeapV(ai.npcCombat);

                        // Veredicto: prioriza la CAUSA TRANSVERSAL (a) sobre la del ataque concreto (b).
                        // EN: Verdict: prioritizes the CROSS-CUTTING cause (a) over the concrete attack cause (b).
                        const char* aiVerdict2;
                        if (!ai.hostRead || ai.hostChar == 0) {
                            aiVerdict2 = "host no legible (primaryChar null / AV)";
                        } else if (!hostHasAI) {
                            aiVerdict2 = "*** host SIN AI (char+0x650=NULL) -> el char del host NO tiene "
                                         "subsistema de IA (spawn MP no recorrio initAISubsystems). Sin AI no "
                                         "piensa ni combate. Lo cubre la cadena de activacion/0x62B210 ***";
                        } else if (ai.hostAIVtblOk == 0) {
                            aiVerdict2 = "host char+0x650 != vtable AI conocida (0x16FA3E8) -> objeto/offset "
                                         "inesperado en +0x650 (re-examinar; NO asumir AI en este char)";
                        } else if (!hostHasPlatoon && npcHasPlatoon) {
                            aiVerdict2 = "*** CAUSA TRANSVERSAL (a): host AI+0x10 (Platoon registrado) = 0/basura "
                                         "mientras el NPC sano lo tiene != 0 -> el Tasker del host queda HUERFANO: "
                                         "el AI tick no consume NINGUN Task (atacar/levantarse/curar) -> amIdle=1 "
                                         "permanente. Lo repara [FIX-PLATOON] (setActivePlatoon 0x6213F0) ***";
                        } else if (!hostHasPlatoon) {
                            aiVerdict2 = "host AI+0x10 (Platoon) = 0/basura; NPC sin dato comparable -> sospechoso "
                                         "(IA huerfana). Confirmar con un NPC vecino legible / [FIX-PLATOON]";
                        } else if (!hostHasCombat && npcHasCombat) {
                            aiVerdict2 = "*** CAUSA (b): host CombatClass (*(CharBody+0x8)) = 0 mientras el NPC sano "
                                         "la tiene != 0 -> el AI/Platoon estan OK (no amIdle transversal) pero el "
                                         "ATAQUE concreto falla por falta de CombatClass. Lo repara [FIX-COMBATCLASS] ***";
                        } else if (ai.hostAiTask == 0) {
                            aiVerdict2 = "host con AI/Platoon/CombatClass pero AI+0x20 (AITaskSytem) = NULL -> el "
                                         "manager de cola de tareas falta (raro si AI valido). Re-examinar AI::create";
                        } else if (ai.hostJobs == 0 && ai.npcFound && ai.npcJobs > 0) {
                            aiVerdict2 = "host con cluster IA COMPLETO pero cola de jobs VACIA (=0) mientras el NPC "
                                         "vecino tiene jobs>0 -> las ordenes/jobs NO se materializan en ESTE char "
                                         "(encolado/consumo). Cruzar con [DIAG-ATTACK] amIdle/activeTask";
                        } else {
                            aiVerdict2 = "host con AI+Platoon+CombatClass+AITaskSytem validos y jobs>=0 igual que el "
                                         "NPC -> la IA/cola del host esta OK; si no actua, la causa es OTRA capa";
                        }

                        // Log HOST vs NPC LADO A LADO con las 3 sondas (Platoon AI+0x10, CombatClass, IndivTask).
                        // EN: Log HOST vs NPC SIDE BY SIDE with the 3 probes (Platoon AI+0x10, CombatClass, IndivTask).
                        spdlog::info(
                            "[DIAG-AITASK] HOST char=0x{:X} | AI(char+0x650)=0x{:X} vtblOk={} | "
                            "Platoon(AI+0x10)=0x{:X} [{}] | aiTask(AI+0x20)=0x{:X} vtblOk={} jobs(+0x2F0)={} "
                            "cap={} data=0x{:X} | CombatClass(*(body+8))=0x{:X} [{}] body(+0x648)=0x{:X} | "
                            "anim(+0x448)=0x{:X} indivTask(+0xE8)=0x{:X} run(vt+0x10)=0x{:X} (read={})",
                            ai.hostChar, ai.hostAI, ai.hostAIVtblOk,
                            ai.hostPlatoon, hostHasPlatoon ? "OK" : "AUSENTE",
                            ai.hostAiTask, ai.hostVtblOk, ai.hostJobs, ai.hostJobsCap, ai.hostJobsData,
                            ai.hostCombat, hostHasCombat ? "OK" : "AUSENTE", ai.hostBody,
                            ai.hostAnim, ai.hostIndivTask, ai.hostIndivRun, ai.hostRead);
                        spdlog::info(
                            "[DIAG-AITASK] NPC  char=0x{:X} name='{}' | AI(+0x650)=0x{:X} vtblOk={} | "
                            "Platoon(AI+0x10)=0x{:X} [{}] | aiTask(AI+0x20)=0x{:X} vtblOk={} jobs(+0x2F0)={} "
                            "cap={} data=0x{:X} | CombatClass=0x{:X} [{}] body=0x{:X} | "
                            "anim=0x{:X} indivTask=0x{:X} run=0x{:X} (found={})",
                            ai.npcChar, ai.npcName, ai.npcAI, ai.npcAIVtblOk,
                            ai.npcPlatoon, npcHasPlatoon ? "OK" : "AUSENTE",
                            ai.npcAiTask, ai.npcVtblOk, ai.npcJobs, ai.npcJobsCap, ai.npcJobsData,
                            ai.npcCombat, npcHasCombat ? "OK" : "AUSENTE", ai.npcBody,
                            ai.npcAnim, ai.npcIndivTask, ai.npcIndivRun, ai.npcFound);
                        spdlog::info("[DIAG-AITASK] ==> {}", aiVerdict2);

                        // ── [DIAG-REMOTE] ¿El mod marcó al HOST como remote-controlled? ──────
                        // HIPÓTESIS (audit puppet): el char LOCAL del host ('Dani', data[0]) podría
                        // estar metido por error en el set s_remoteControlled de ai_hooks (flujo de
                        // spawn/claim/sync que lo trató como entidad de red). Si así fuera, el mod
                        // estaría BLOQUEANDO su combate (Hook_ApplyDamage hace `return` si el atacante
                        // es remote-controlled) Y su movimiento (movement_hooks gatea por IsRemoteControlled).
                        // Eso explicaría char vivo + en simlist + con reloj corriendo pero INERTE.
                        //
                        // Veredicto DISCRIMINANTE:
                        //   host=remote-controlled = SÍ  → CAUSA CONFIRMADA (desmarcar el char del host).
                        //   host=NO + setSize=0          → el host NO está marcado; la causa es OTRA capa.
                        //   host=NO + NPC vecino=NO      → coherente (ni host ni NPC local son puppets).
                        // Comparamos host vs NPC vecino (mismo char que [DIAG-AITASK], de la simlist).
                        // SOLO LECTURA y SEGURO SIN SEH: IsRemoteControlled/RemoteControlledCount
                        // hacen lookup en un std::unordered_set por VALOR de puntero — NO derefencian
                        // memoria del juego, así que no pueden provocar AV (mismo motivo por el que el
                        // [DIAG-COMBAT] de game_character.cpp ya las llama sin __try). No se mete __try
                        // aquí porque este scope contiene objetos C++ con destructor (snapshots) y MSVC
                        // prohíbe mezclar __try con unwinding de objetos en la misma función (C2712).
                        // EN: ── [DIAG-REMOTE] Did the mod mark the HOST as remote-controlled? ── HYPOTHESIS (puppet audit): the
                        //     host LOCAL char (data[0]) could have been put by mistake in the ai_hooks s_remoteControlled set; the
                        //     mod would then BLOCK its combat and movement. Verdict: host remote-controlled = YES → CAUSE (unmark
                        //     it); host NO → the cause is another layer. READ ONLY and SAFE WITHOUT SEH: IsRemoteControlled /
                        //     RemoteControlledCount do a std::unordered_set lookup by pointer VALUE — they never dereference game
                        //     memory. No __try here because this scope holds C++ objects with destructors (C2712).
                        {
                            void* hostPtr = primaryChar; // char del host que usa el mod (PI+0x2B0 data[0])
                            // EN: hostPtr = host char used by the mod (PI+0x2B0 data[0]); npcPtr = neighbor simlist NPC.
                            void* npcPtr  = reinterpret_cast<void*>(ai.npcChar); // NPC vecino de la simlist
                            bool   hostRemote = (hostPtr != nullptr)
                                                && ai_hooks::IsRemoteControlled(hostPtr);
                            bool   npcRemote  = ai.npcFound && (npcPtr != nullptr)
                                                && ai_hooks::IsRemoteControlled(npcPtr);
                            size_t setSize    = ai_hooks::RemoteControlledCount();

                            const char* remoteVerdict;
                            if (hostPtr == nullptr) {
                                remoteVerdict = "host null este frame — reintentar";
                            } else if (hostRemote) {
                                remoteVerdict = "*** HOST MARCADO como remote-controlled -> el mod lo trata "
                                                "como puppet. NOTA: hoy ApplyDamage/MoveTo/SetPosition NO "
                                                "están hookeados (mov-rax-rsp), así que el bloqueo es código "
                                                "MUERTO; aun así es una ANOMALÍA a corregir (desmarcar host) ***";
                            } else if (setSize == 0) {
                                remoteVerdict = "host NO remote + set VACÍO -> el host no es puppet; "
                                                "la causa del combate congelado es OTRA capa";
                            } else {
                                remoteVerdict = "host NO remote (set tiene otros chars: peers/NPC hijack) "
                                                "-> el host no es puppet; causa en OTRA capa";
                            }

                            spdlog::info(
                                "[DIAG-REMOTE] host=0x{:X} IsRemoteControlled={} | NPCvecino=0x{:X} "
                                "IsRemoteControlled={} (found={}) | setSize={} ==> {}",
                                reinterpret_cast<uintptr_t>(hostPtr), hostRemote,
                                ai.npcChar, npcRemote, ai.npcFound,
                                setSize, remoteVerdict);
                        }

                        // ── [DIAG-PCTRL] ¿El host está "controlado por el jugador" según el MOTOR? ──
                        // CONTEXTO DE LA PISTA: el spec viejo (2026-04-19) suponía un flag escribible
                        // 'playerControlled' en el Character que, si valía 0, dejaba al char "conducido
                        // por la IA, sin responder a órdenes". COINCIDÍA con el síntoma del host inerte.
                        //
                        // VERIFICADO POR RE (binario 1.0.68, 2026-06-17/18 + KenshiLib Character.h):
                        //   La clase Character NO TIENE ningún campo playerControlled/isPlayerControlled.
                        //   El offset está marcado N/A (-2) y WritePlayerControlled() es CÓDIGO MUERTO
                        //   (siempre return false). El motor decide quién es "player-controlled" por
                        //   FACCIÓN: un char responde a tus órdenes si  char.faction(+0x10) == playerFaction
                        //   (GameWorld+0x580 -> +0x2A0). Esa igualdad ES la semántica real del flag que el
                        //   spec creía un bool. El char ACTIVO concreto es PI+0x2A8 (controlledChar).
                        //
                        // Por eso este DIAG NO vuelca un flag inexistente: vuelca el MECANISMO REAL.
                        //   host.faction == playerFaction  ->  el motor te trata como jugador (acepta órdenes).
                        //   host.faction != playerFaction  ->  *** el motor te ignora: char "ni jugador ni IA
                        //                                       propia" -> INERTE. ESA sería la causa. ***
                        // Comparamos host vs NPC vecino (control): el NPC DEBE dar != (es NPC); si el NPC
                        // diera == habría que dudar de la player-faction resuelta.
                        // SOLO LECTURA. Memory::Read ya va protegido internamente (SEH); sin __try aquí por
                        // C2712 (este scope tiene objetos C++ con destructor — igual que [DIAG-REMOTE]).
                        // EN: ── [DIAG-PCTRL] Is the host "player controlled" according to the ENGINE? ── The old spec assumed a
                        //     writable 'playerControlled' flag in the Character. VERIFIED BY RE (1.0.68 + KenshiLib Character.h):
                        //     Character has NO such field; the offset is N/A (-2) and WritePlayerControlled() is DEAD CODE (always
                        //     false). The engine decides "player controlled" by FACTION: a char obeys your orders if
                        //     char.faction(+0x10) == playerFaction (GameWorld+0x580 -> +0x2A0); the ACTIVE char is PI+0x2A8. So this
                        //     DIAG dumps the REAL mechanism: host.faction == playerFaction → accepts orders; != → the engine ignores
                        //     it (INERT). The neighbor NPC is the control (must be !=). READ ONLY (Memory::Read has internal SEH).
                        {
                            const int facOff = game::GetOffsets().character.faction; // +0x10 (VERIFICADO)
                            uintptr_t playerFaction = game::GetPlayerFactionDirect(); // GW+0x580->+0x2A0

                            void*     hostPtr2  = primaryChar;                 // char del host (data[0])
                            void*     npcPtr2   = reinterpret_cast<void*>(ai.npcChar); // NPC vecino simlist
                            uintptr_t hostFac   = 0;
                            uintptr_t npcFac    = 0;
                            bool      hostFacOk = (hostPtr2 != nullptr)
                                                  && Memory::Read(reinterpret_cast<uintptr_t>(hostPtr2) + facOff, hostFac);
                            bool      npcFacOk  = ai.npcFound && (npcPtr2 != nullptr)
                                                  && Memory::Read(reinterpret_cast<uintptr_t>(npcPtr2) + facOff, npcFac);

                            // -1 = no comparable; 1 = es player-controlled (faction match); 0 = no lo es.
                            // EN: -1 = not comparable; 1 = player controlled (faction match); 0 = not.
                            int hostIsPlayerCtl = (!hostFacOk || playerFaction == 0) ? -1
                                                 : (hostFac == playerFaction ? 1 : 0);
                            int npcIsPlayerCtl  = (!npcFacOk  || playerFaction == 0) ? -1
                                                 : (npcFac == playerFaction ? 1 : 0);

                            const char* pctrlVerdict;
                            if (playerFaction == 0) {
                                pctrlVerdict = "playerFaction no resuelta (GW+0x580->+0x2A0=0) — reintentar";
                            } else if (!hostFacOk) {
                                pctrlVerdict = "host.faction no legible este frame — reintentar";
                            } else if (hostIsPlayerCtl == 1) {
                                pctrlVerdict = "host ES player-controlled (faction==playerFaction) -> el MOTOR "
                                               "lo reconoce como jugador y ACEPTA órdenes. La inercia NO viene "
                                               "del 'playerControlled' (que no existe): mirar el sistema de "
                                               "tareas/encolado de jobs ([DIAG-AITASK])";
                            } else if (hostIsPlayerCtl == 0) {
                                pctrlVerdict = "*** host NO player-controlled: host.faction != playerFaction -> "
                                               "el motor NO lo trata como jugador y RECHAZA sus órdenes -> char "
                                               "INERTE (ni jugador ni IA propia). FIX CANDIDATO: "
                                               "FixCharacterFactionTo(host, playerFaction) ***";
                            } else {
                                pctrlVerdict = "no comparable";
                            }

                            spdlog::info(
                                "[DIAG-PCTRL] facOff=+0x{:X} playerFaction=0x{:X} | HOST=0x{:X} "
                                "faction=0x{:X} isPlayerCtl(==playerFac)={} (read={}) | NPCvecino=0x{:X} "
                                "faction=0x{:X} isPlayerCtl={} (read={}) ==> {}",
                                facOff, playerFaction,
                                reinterpret_cast<uintptr_t>(hostPtr2), hostFac, hostIsPlayerCtl, hostFacOk,
                                ai.npcChar, npcFac, npcIsPlayerCtl, npcFacOk,
                                pctrlVerdict);
                            spdlog::info(
                                "[DIAG-PCTRL] NOTA: 'playerControlled' NO es un flag del Character en 1.0.68 "
                                "(RE confirmado). WritePlayerControlled() es no-op. El equivalente real es la "
                                "igualdad de facción de arriba + controlledChar(PI+0x2A8). Re-activar "
                                "WritePlayerControlled en Path C NO arreglaría nada.");
                        }

                        // ── [DIAG-COMBATSTRUCT] Cluster IA/combate del Character (offsets CORREGIDOS) ──
                        // Sustituye al [DIAG-AITASK] de arriba, que leía char+0x20 (FALSO: es el buffer
                        // SSO del name). Cadena REAL confirmada por RE de bytes (Steam 1.0.68):
                        //   char+0x640 -> CharMovement (camina)   char+0x650 -> AI -> AI+0x20 -> AITaskSytem.
                        // Vuelca host vs NPC vecino. Veredicto = la hipótesis del usuario (host sin estructura
                        // de combate inicializada) confirmada/refutada con los offsets correctos. SOLO LECTURA.
                        // EN: ── [DIAG-COMBATSTRUCT] Character AI/combat cluster (CORRECTED offsets) ── Replaces the [DIAG-AITASK]
                        //     char+0x20 read (FALSE: SSO name buffer). Real chain (byte RE Steam 1.0.68): char+0x640 ->
                        //     CharMovement (walks), char+0x650 -> AI -> AI+0x20 -> AITaskSytem. Dumps host vs neighbor NPC and
                        //     confirms/refutes "host without initialized combat structure". READ ONLY.
                        {
                            CombatStructSnapshot cs;
                            SEH_ReadCombatStructDiag(modBase, gwObj, primaryChar, &cs);

                            // Veredicto comparando el cluster del host vs el del NPC vecino.
                            // EN: Verdict comparing the host cluster with the neighbor NPC one (missing AI, missing AITaskSytem,
                            //     missing CharBody, missing CombatClass = CAUSE A, unexpected vtables, empty job queue, or all OK).
                            const char* csVerdict;
                            if (!cs.hostRead || cs.hostChar == 0) {
                                csVerdict = "host no legible (primaryChar null / AV)";
                            } else if (cs.hostAI == 0) {
                                csVerdict = "*** CAUSA CONFIRMADA: host SIN AI (char+0x650=NULL) -> el spawn "
                                            "del mod NO recorrio initAISubsystems (0x62AF50). Sin subsistema "
                                            "de IA: no piensa, no combate, no es objetivo. CAMINA porque "
                                            "CharMovement(char+0x640) si existe. FIX: invocar la cadena de "
                                            "activacion (0x62B210/initAISubsystems) al reclamar el char ***";
                            } else if (cs.hostAIVtblOk == 0) {
                                csVerdict = "host char+0x650 != vtable AI conocida -> objeto inesperado en "
                                            "+0x650 (re-examinar; NO asumir AI en este char)";
                            } else if (cs.hostAiTask == 0) {
                                csVerdict = "*** CAUSA CONFIRMADA: host tiene AI pero SIN AITaskSytem "
                                            "(AI+0x20=NULL) -> el spawn del mod salto AI::create (0x622110). "
                                            "Sin manager de tareas las ordenes de ataque no se procesan. "
                                            "FIX: llamar AI::create sobre el AI del host ***";
                            } else if (cs.hostTaskVtblOk == 0) {
                                csVerdict = "host AI+0x20 != vtable AITaskSytem conocida -> objeto inesperado "
                                            "(re-examinar la cadena +0x650/+0x20)";
                            } else if (cs.hostBody == 0) {
                                csVerdict = "*** CAUSA A (variante extrema): host SIN CharBody (char+0x648=NULL) "
                                            "-> giveBirth/createComponents (0x62AF50/0x62B210) NO corrieron. "
                                            "Sin CharBody no hay CombatClass: el char no puede atacar ***";
                            } else if (cs.hostCombat == 0) {
                                // ── LA SONDA DECISIVA (audit-12): CombatClass = *(CharBody+0x8) ──
                                // EN: ── THE DECISIVE PROBE (audit-12): CombatClass = *(CharBody+0x8) ──
                                csVerdict = "*** CAUSA A CONFIRMADA: host SIN CombatClass (*(CharBody+0x8)=NULL) "
                                            "-> el spawn del mod entro por process (0x581770) con descriptor de "
                                            "tipo != 1, asi que SALTO giveBirth->CharBody::create (0x621460). "
                                            "El char tiene AI/AITaskSytem/CharBody pero CombatClass=NULL: CAMINA/"
                                            "MINA/ENTRENA (GOAP) pero NO ATACA (atacar usa CombatClass: tick "
                                            "0x5C67C0, attackTarget 0x5CB0A0, currentTarget@+0x290). "
                                            "FIX: [FIX-COMBATCLASS] llamar CharBody::create sobre el char ***";
                            } else if (cs.hostCombatVtblOk == 0) {
                                csVerdict = "host *(CharBody+0x8) != vtable CombatClass conocida (0x16F67B8) -> "
                                            "objeto inesperado en CharBody+0x8 (re-examinar; NO asumir CombatClass)";
                            } else if (cs.npcFound && cs.npcAiTask != 0 &&
                                       cs.hostJobs == 0 && cs.npcJobs > 0) {
                                csVerdict = "host con cluster IA COMPLETO pero cola de jobs VACIA (=0) mientras "
                                            "el NPC vecino tiene jobs>0 -> las ordenes no llegan a ESTE char "
                                            "(encolado), NO falta de estructura. Mirar capa de ordenes/Tasker";
                            } else {
                                csVerdict = "host con cluster IA/combate COMPLETO (CharMovement+AI+AITaskSytem+"
                                            "CombatClass validos) -> la estructura de combate SI esta inicializada. "
                                            "La causa NO es esta: revisar gate GW+0x8B9 (combate congelado por "
                                            "rama paused) ya documentado, o que primaryChar no sea el char "
                                            "realmente controlado (CAUSA B)";
                            }

                            // Veredicto explicito de la SONDA host-vs-NPC (CausaA si difieren).
                            // EN: Explicit verdict of the host-vs-NPC PROBE (CauseA if they differ).
                            const char* attackVerdict;
                            if (cs.hostRead && cs.npcFound) {
                                if (cs.hostCombat == 0 && cs.npcCombat != 0)
                                    attackVerdict = "host CombatClass==0 && NPC CombatClass!=0 ==> CAUSA A CONFIRMADA";
                                else if (cs.hostCombat != 0 && cs.npcCombat != 0)
                                    attackVerdict = "host y NPC con CombatClass != 0 ==> NO es CausaA (mirar gate/CausaB)";
                                else
                                    attackVerdict = "lectura no concluyente (NPC sin CombatClass legible?)";
                            } else {
                                attackVerdict = "sin NPC de control legible para comparar";
                            }

                            spdlog::info(
                                "[DIAG-COMBATSTRUCT] HOST char=0x{:X} | move(+0x640)=0x{:X} vtblOk={} | "
                                "AI(+0x650)=0x{:X} vtblOk={} | aiTask(AI+0x20)=0x{:X} vtblOk={} | "
                                "jobs(+0x2F0)={} (read={})",
                                cs.hostChar, cs.hostMovement, cs.hostMoveVtblOk,
                                cs.hostAI, cs.hostAIVtblOk, cs.hostAiTask, cs.hostTaskVtblOk,
                                cs.hostJobs, cs.hostRead);
                            spdlog::info(
                                "[DIAG-COMBATSTRUCT] NPC  char=0x{:X} name='{}' | move=0x{:X} vtblOk={} | "
                                "AI=0x{:X} vtblOk={} | aiTask=0x{:X} vtblOk={} | jobs={} (found={})",
                                cs.npcChar, cs.npcName, cs.npcMovement, cs.npcMoveVtblOk,
                                cs.npcAI, cs.npcAIVtblOk, cs.npcAiTask, cs.npcTaskVtblOk,
                                cs.npcJobs, cs.npcFound);
                            spdlog::info("[DIAG-COMBATSTRUCT] ==> {}", csVerdict);

                            // ── [DIAG-ATTACK] Capa de combate: la SONDA decisiva (CombatClass) ──
                            // CombatClass = *(char+0x648 [CharBody] + 0x8). vtblOk vs modBase+0x16F67B8.
                            // Task activo = *(char+0x448+0xE8); vtbl vs Tasker 0x16BDC68 / Task_MeleeAttack
                            // 0x16BE448 / Task_FocusedMeleeAttack 0x16BF9E8. currentTarget = CombatClass+0x290
                            // (escrito por setAttackTarget 0x665580).
                            // EN: ── [DIAG-ATTACK] Combat layer: the decisive PROBE (CombatClass) ── CombatClass = *(char+0x648
                            //     [CharBody] + 0x8), vtblOk vs modBase+0x16F67B8. Active Task vtbl vs Tasker 0x16BDC68 /
                            //     Task_MeleeAttack 0x16BE448 / Task_FocusedMeleeAttack 0x16BF9E8. currentTarget = CombatClass+0x290
                            //     (written by setAttackTarget 0x665580).
                            //     NOTE (inconsistency): the line above still says the active Task is *(char+0x448+0xE8); the code
                            //     (SEH_ReadOneCombatStruct) actually reads *(CharBody+0x68) since the 2026-06-19 correction.
                            // ES: NOTA (incoherencia): la línea de arriba sigue diciendo que el Task activo es *(char+0x448+0xE8); el
                            //     código (SEH_ReadOneCombatStruct) lee en realidad *(CharBody+0x68) desde la corrección del 2026-06-19.
                            const uintptr_t taskerVtblAbs = modBase + CombatStructSnapshot::kTaskerVtableRVA;
                            const uintptr_t meleeVtblAbs  = modBase + CombatStructSnapshot::kTaskMeleeVtableRVA;
                            // Ataque ORDENADO por el jugador (clic manual): tarea distinta a la automática (RE 2026-07-14).
                            // EN: Player-ORDERED attack (manual click): a different task from the automatic one (RE 2026-07-14).
                            const uintptr_t focusedMeleeVtblAbs = modBase + CombatStructSnapshot::kTaskFocusedMeleeVtableRVA;
                            auto taskName = [&](uintptr_t v) -> const char* {
                                if (v == 0) return "none";
                                if (v == meleeVtblAbs)        return "Task_MeleeAttack(auto)";
                                if (v == focusedMeleeVtblAbs) return "Task_FocusedMeleeAttack(ordenado)";
                                if (v == taskerVtblAbs) return "Tasker(base)";
                                return "otro";
                            };
                            spdlog::info(
                                "[DIAG-ATTACK] HOST anim(+0x448)=0x{:X} | body(+0x648)=0x{:X} vtblOk={} | "
                                "CombatClass(*(body+8))=0x{:X} vtblOk={} | currentTarget(+0x290)=0x{:X} | "
                                "activeTask(body+0x68)=0x{:X} vtbl=0x{:X} [{}] | amIdle(body+0x70)={}",
                                cs.hostAnim, cs.hostBody, cs.hostBodyVtblOk,
                                cs.hostCombat, cs.hostCombatVtblOk, cs.hostTarget,
                                cs.hostActiveTask, cs.hostActiveTaskVtbl, taskName(cs.hostActiveTaskVtbl),
                                cs.hostAmIdle);
                            spdlog::info(
                                "[DIAG-ATTACK] NPC  anim=0x{:X} | body=0x{:X} vtblOk={} | "
                                "CombatClass=0x{:X} vtblOk={} | currentTarget=0x{:X} | "
                                "activeTask(body+0x68)=0x{:X} vtbl=0x{:X} [{}] | amIdle={}",
                                cs.npcAnim, cs.npcBody, cs.npcBodyVtblOk,
                                cs.npcCombat, cs.npcCombatVtblOk, cs.npcTarget,
                                cs.npcActiveTask, cs.npcActiveTaskVtbl, taskName(cs.npcActiveTaskVtbl),
                                cs.npcAmIdle);
                            spdlog::info("[DIAG-ATTACK] ==> {}", attackVerdict);
                        }

                        // ── [DIAG-HOSTILITY] ¿Se computa la hostilidad host<->bandidos? ──────────
                        // CAUSA RAÍZ CANDIDATA del combate co-op congelado SIMÉTRICO. El cluster
                        // IA/combate del host está COMPLETO (lo confirma [DIAG-COMBATSTRUCT]), así
                        // que el host NO carece de estructura. El síntoma exacto (host no ataca a
                        // bandidos Y bandidos no le atacan = simétrico) apunta a que la HOSTILIDAD
                        // MUTUA no se computa.
                        //
                        // RE de bytes (Steam 1.0.68): la hostilidad la decide isEnemy(0x6B26D0) =
                        // (relacion_float <= -30.0) entre Faction objects, NO el fundamental type
                        // (REFUTA audit-05). Las relaciones viven en un unordered_map MSVC embebido
                        // en Faction+0x78 (FactionRelations*). Una facción "Player N" clonada del FCS
                        // puede nacer con ese map VACÍO -> toda relación resuelve a 0.0 (neutro) ->
                        // -30 < 0 < +50 -> ni enemigo ni aliado con NADIE -> nadie pelea.
                        //
                        // Volcamos la relación host<->NPC vecino en AMBOS sentidos (read-only, dos
                        // métodos que se validan: lineal vs hash) + el nº de relaciones de cada
                        // facción. Veredicto auto-discriminante. SOLO LECTURA, SEH dentro del helper.
                        // EN: ── [DIAG-HOSTILITY] Is host<->bandit hostility computed? ── CANDIDATE ROOT CAUSE of the SYMMETRIC
                        //     frozen co-op combat. The host AI/combat cluster is COMPLETE ([DIAG-COMBATSTRUCT]); the exact symptom
                        //     (host does not attack bandits AND bandits do not attack it) points to MUTUAL HOSTILITY not being
                        //     computed. Byte RE: hostility is decided by isEnemy (0x6B26D0) = (float relation <= -30.0) between
                        //     Faction objects, NOT by fundamental type (refutes audit-05). A cloned "Player N" faction can be born
                        //     with an EMPTY relation map → everything 0.0 → neither enemy nor ally with anyone. Dumps the host<->NPC
                        //     relation BOTH ways + relation counts + native isEnemy. READ ONLY, SEH inside the helper.
                        {
                            uintptr_t playerFaction = game::GetPlayerFactionDirect(); // GW+0x580->+0x2A0
                            HostilityDiagSnapshot hs;
                            SEH_ReadHostilityDiag(gwObj, primaryChar, playerFaction, modBase, &hs);

                            // Prueba DEFINITIVA: la que dicta el motor (isEnemy nativo). Prevalece
                            // sobre el lookup manual del map. Tras el FIX (default=-100) debe dar 1.
                            // EN: DEFINITIVE test: the one the engine dictates (native isEnemy). Overrides the manual map lookup.
                            //     After the FIX (default=-100) it must be 1.
                            bool nativeHostMutuo = (hs.hostEnemyNpc == 1 && hs.npcEnemyHost == 1);

                            const char* hVerdict;
                            if (!hs.read || hs.hostFaction == 0) {
                                hVerdict = "host.faction no resuelta este frame — reintentar";
                            } else if (!hs.hostFROk) {
                                hVerdict = "Faction+0x78 (FactionRelations*) del host no legible — "
                                           "revisar offset 0x78 o puntero de facción";
                            } else if (!hs.npcFound) {
                                hVerdict = "no se halló NPC vecino de OTRA facción en la simlist — "
                                           "no hay con quién comparar hostilidad (reintentar / acercarse "
                                           "a un bandido)";
                            } else if (hs.hostEnemyNpc == 1) {
                                hVerdict = "*** FIX OK (segun isEnemy NATIVO): el host VE al NPC como "
                                           "ENEMIGO (defaultRelation<=-30). El gate de ataque debe permitir "
                                           "encolar la orden. Si aun asi el char no ataca, mirar la capa de "
                                           "Tasker (DIAG-ATTACK: amIdle/activeTask) o el gate GW+0x8B9 ***";
                            } else if (hs.hostEnemyNpc == 0) {
                                hVerdict = "*** isEnemy NATIVO dice host NO-enemigo del NPC: el FIX no se ha "
                                           "aplicado todavia (defaultRelation del host != -100) o el NPC tiene "
                                           "una entry explicita no-hostil. Ver hostDefaultRel abajo: si != -100, "
                                           "el FIX-HOSTILITY aun no corrio (espera al siguiente tick) ***";
                            } else if (hs.hostRelCount == 0) {
                                hVerdict = "facción del host con map de relaciones VACÍO (relCount=0). Es lo "
                                           "esperado: el FIX no puebla el map, fija defaultRelation=-100. "
                                           "Verificar hostDefaultRel y hostEnemyNpc (isEnemy nativo)";
                            } else {
                                hVerdict = "estado intermedio — guiarse por isEnemy NATIVO (hostEnemyNpc/"
                                           "npcEnemyHost) y por hostDefaultRel, no por el lookup manual del map";
                            }

                            // isEnemy nativo: -1=no llamado, 0=NO, 1=SÍ.
                            // EN: Native isEnemy: -1 = not called, 0 = NO, 1 = YES.
                            auto enemyStr = [](int v) -> const char* {
                                return v == 1 ? "SÍ" : (v == 0 ? "NO" : "n/a");
                            };

                            spdlog::info(
                                "[DIAG-HOSTILITY] HOST char=0x{:X} faction=0x{:X} FR(+0x78)=0x{:X} "
                                "ok={} relCount(+0x40 boost)={} defaultRel(+0x60)={:.2f}",
                                hs.hostChar, hs.hostFaction, hs.hostFR, hs.hostFROk,
                                hs.hostRelCount, hs.hostDefaultRel);
                            spdlog::info(
                                "[DIAG-HOSTILITY] NPC  char=0x{:X} name='{}' faction=0x{:X} FR=0x{:X} "
                                "ok={} relCount={} (found={})",
                                hs.npcChar, hs.npcName, hs.npcFaction, hs.npcFR, hs.npcFROk,
                                hs.npcRelCount, hs.npcFound);
                            spdlog::info(
                                "[DIAG-HOSTILITY] isEnemy NATIVO(0x6B26D0): host->npc={} | npc->host={} "
                                "(called={}) | lookup manual host->npc={:.2f} npc->host={:.2f} (informativo)",
                                enemyStr(hs.hostEnemyNpc), enemyStr(hs.npcEnemyHost), hs.isEnemyCalled,
                                hs.host2npc, hs.npc2host);
                            spdlog::info(
                                "[DIAG-HOSTILITY] umbrales: enemigo<=-30 aliado>=+50 | hostilMutuo(nativo)={} "
                                "==> {}",
                                nativeHostMutuo, hVerdict);
                        }
                    }
                }
            }

            // ── [PISTA MUERTA — GW+0x8B8] NO perseguir este byte ────────────────────
            // El FIX-SIM anterior limpiaba GW+0x8B8 creyéndolo el guard de reentrancia de
            // updateCharacters pegado en 1. La doble verificación RE (bytes, 2026-06-18) lo
            // REFUTÓ: GW+0x8B8 se ESCRIBE (=1 al entrar updateCharacters 0x786E6F, =0 al salir
            // 0x7871FA) pero NO se LEE en NINGÚN punto del binario → no gobierna nada.
            // Limpiarlo era INOCUO pero NO arreglaba el combate. Eliminado. El guard real de
            // reentrancia es GW+0x749, y gobierna altas/bajas DIFERIDAS de la lista de chars
            // (GW+0x750), no el AI tick. El gate que SÍ decide si corre el AI tick es GW+0x8B9
            // (ver [FIX-SIM] abajo), leído por mainLoop en 0x788FF5.
            // EN: ── [DEAD LEAD — GW+0x8B8] Do NOT chase this byte ── The old FIX-SIM cleared GW+0x8B8 believing it was
            //     the updateCharacters reentrancy guard stuck at 1. Byte RE (2026-06-18) REFUTED it: GW+0x8B8 is WRITTEN
            //     (=1 entering updateCharacters, =0 leaving) but never READ → governs nothing. Removed. The real
            //     reentrancy guard is GW+0x749 (deferred add/remove of the char list GW+0x750), not the AI tick. The
            //     gate that DOES decide whether the AI tick runs is GW+0x8B9 (see [FIX-SIM] below).

            // ── Cuántos otros jugadores hay en la partida ──
            // GetRemoteCount() > 0 ⇒ no estamos solos: la sim no puede quedar pausada para
            // nadie, así que forzamos despausa SIEMPRE. Si es 0 (host solo) respetamos la
            // pausa intencional del jugador, pero AÚN así arreglamos la "pausa fantasma".
            // EN: ── How many other players are in the game ── GetRemoteCount() > 0 ⇒ we are not alone: the sim cannot
            //     stay paused for anyone, so unpausing is ALWAYS forced. If 0 (host alone) the player intentional pause
            //     is respected, but the "ghost pause" is still fixed.
            size_t remoteCount     = m_entityRegistry.GetRemoteCount();
            bool   multiplayerActive = (remoteCount > 0);

            // ── [FIX-SIM] Causa raíz REAL del combate congelado: gate GW+0x8B9 pegado ─
            // CONFIRMADO por RE de bytes (Fase 4, doble verificación 2026-06-18):
            //   mainLoop (0x788A00) en 0x788FF5 hace  cmp byte[GW+0x8B9],0 ; jne paused.
            //   Con GW+0x8B9 != 0 → SALTA updateCharacters → ningún char recibe el AI tick
            //   [vtbl+0xE8] (combate/Jobs/levantarse/recuperar KO). El reloj y el movimiento
            //   van por otra rama (tick reducido [vtbl+0x270]) → "reloj corre, chars inertes".
            //
            // Por qué se queda pegado: el setter oficial 0x787D40 escribe
            //   GW+0x8B9 = argBool OR (gameSpeed[GW+0x700] == 0.0f exacto).
            // Si el host pausó con barra espaciadora (gameSpeed=0.0), llamar SetPaused(false)
            // NO basta: el OR re-pega el flag a 1 mientras gameSpeed siga clavado en 0.0.
            //
            // FIX: en CUALQUIER caso en que vayamos a despausar (pausa fantasma o multiplayer),
            // restauramos gameSpeed a 1.0 ANTES de llamar al setter, para que el OR no dispare
            // y la despausa pase limpia. En host SOLO con pausa intencional (gameSpeed==0 sin
            // otros jugadores y sin desajuste de cache) NO tocamos nada: el jugador manda.
            //
            // Nota de seguridad: writes de 1 byte (gate) / 4 bytes (float) sobre el objeto
            // GameWorld ya resuelto (GetWorldObject), todos bajo SEH. Sin objetos C++ en __try.
            // EN: ── [FIX-SIM] REAL root cause of the frozen combat: GW+0x8B9 gate stuck ── (byte RE, Phase 4)
            //     mainLoop (0x788A00) at 0x788FF5 does cmp byte[GW+0x8B9],0; jne paused. With GW+0x8B9 != 0 it SKIPS
            //     updateCharacters → no char gets the AI tick [vtbl+0xE8]; clock and movement go through another branch
            //     ([vtbl+0x270]) → "clock runs, chars inert". Why it sticks: the official setter 0x787D40 writes
            //     GW+0x8B9 = argBool OR (gameSpeed[GW+0x700] == 0.0f); if the host paused with space (gameSpeed=0.0),
            //     SetPaused(false) is NOT enough. FIX: whenever we are going to unpause (ghost pause or multiplayer),
            //     restore gameSpeed to 1.0 BEFORE calling the setter. Host ALONE with an intentional pause: untouched.
            //     Safety: 1-byte (gate) / 4-byte (float) writes on the resolved GameWorld object, all under SEH.

            // Pausa fantasma: la sim corre (pausedRaw==0) pero un subsistema sigue creyendo
            // que está pausado (subsysCache==1).
            // EN: Ghost pause: the sim runs (pausedRaw==0) but a subsystem still believes it is paused
            //     (subsysCache==1).
            bool ghostPause  = (pausedRaw == 0) && (subsysPausedCache == 1);
            bool realPause   = (pausedRaw != 0);
            bool needsUnpause = ghostPause || (realPause && multiplayerActive);

            // ── Restaurar gameSpeed ANTES de despausar (clave del fix del gate) ──
            // Si vamos a despausar pero gameSpeed está EXACTAMENTE en 0.0, el setter oficial
            // 0x787D40 re-pegaría GW+0x8B9 a 1 vía `argBool OR (gameSpeed==0.0)`. Subimos
            // gameSpeed a 1.0 primero para que el OR no dispare y la despausa pase limpia.
            // Solo cuando needsUnpause (no robamos la velocidad de un host que pausó a posta).
            // EN: ── Restore gameSpeed BEFORE unpausing (key of the gate fix) ── If gameSpeed is EXACTLY 0.0 the
            //     official setter would re-stick GW+0x8B9 to 1; raise gameSpeed to 1.0 first. Only when needsUnpause (do
            //     not steal the speed from a host that paused on purpose).
            if (needsUnpause) {
                uintptr_t gwObj = world.GetWorldObject();
                if (gwObj != 0) {
                    uint8_t gate = 0xFF; float spd = -1.f;
                    if (SEH_ReadPauseGate(gwObj, &gate, &spd) && spd == 0.0f) {
                        bool sw = SEH_RestoreGameSpeed(gwObj);
                        static int s_spdRestore = 0;
                        if (++s_spdRestore <= 8) {
                            spdlog::info("[FIX-SIM] gameSpeed==0.0 antes de despausar → restaurado "
                                         "a 1.0 (write={}) para que el setter no re-pegue GW+0x8B9. "
                                         "ghostPause={} mpActive={}", sw ? "OK" : "FALLO",
                                         ghostPause, multiplayerActive);
                        }
                    }
                }
            }

            if (needsUnpause) {
                if (world.SetPaused(false)) {
                    static int s_unpauseCount = 0;
                    if (++s_unpauseCount <= 8) {
                        spdlog::info("Core: Despausa completa (simPaused={}, subsysCache={}, "
                                     "viaOficial={})",
                                     pausedRaw, subsysPausedCache,
                                     game::HasGameSetPausedFn() ? "si" : "no/crudo");
                    }
                } else {
                    static int s_unpauseFailCount = 0;
                    if (++s_unpauseFailCount <= 5) {
                        spdlog::warn("Core: SetPaused(false) FALLÓ");
                    }
                }
            }

            // ── Velocidad: NO pisar la que elige el jugador (host single-player) ──
            // BUG ANTERIOR: forzábamos gameSpeed=1.0 si quedaba fuera de [0.5, 3.5], lo que
            // impedía la velocidad "muy rápida" (>3.5x) y daba sensación de control robado.
            // La velocidad 0.0 ya NO necesita corrección manual: el setter oficial trata
            // gameSpeed==0 como pausa (paused |= speed==0) y al despausar lo reactiva.
            //
            // Solo intervenimos cuando hay MÚLTIPLES jugadores (la sim debe seguir corriendo
            // para todos) y la velocidad quedó EXACTAMENTE en 0 (pausa total local). En ese
            // caso la subimos a 1.0. Con un único jugador (host solo) NO tocamos nada: el
            // host manda sobre su propia velocidad (1x/2x/5x/muy rápida).
            // (remoteCount / multiplayerActive ya calculados arriba para la despausa.)
            // Si es host solo (remoteCount==0) respetamos al 100% la velocidad del jugador,
            // incluida la pausa con barra espaciadora. Solo intervenimos en MP activo.
            // EN: ── Speed: do NOT override the player choice (single-player host) ── OLD BUG: gameSpeed was forced to
            //     1.0 when outside [0.5, 3.5], which blocked "very fast" speed (>3.5x). Speed 0.0 no longer needs manual
            //     correction (the official setter treats it as pause). We only intervene with MULTIPLE players and speed
            //     EXACTLY 0 (full local pause) → raised to 1.0. Host alone: the player speed is respected 100%.
            float currentSpeed = world.GetGameSpeed();
            if (multiplayerActive && currentSpeed == 0.0f) {
                world.WriteGameSpeed(1.0f);
                static int s_speedFixCount = 0;
                if (++s_speedFixCount <= 5) {
                    spdlog::info("Core: gameSpeed==0 con {} entidades remotas → forzado a 1.0 "
                                 "(la sim debe correr para los demás jugadores)", remoteCount);
                }
            }

            // ── [FIX-HOSTILITY] (REPLANTEADO 2026-06-19) defaultRelation hostil en "Player N" ─────
            // Causa raíz del combate co-op congelado SIMÉTRICO: las facciones de jugador "Player N"
            // (en las que acaba el host por el gamestart del .mod) nacen con el map de relaciones
            // VACÍO (Faction+0x78) Y con defaultFactionRelation = 0.0 → isEnemy resuelve "no enemigo"
            // con TODO el mundo → el gate de encolado del ataque rechaza la orden → el char ni se
            // mueve; y los enemigos "huyen" (sin clasificación de aggro). FIX (verificado en bytes):
            //   1. defaultFactionRelation (FactionRelations+0x60) = -100.0 en cada "Player N":
            //      isEnemy(host, X)=true para toda X sin entry específica (isEnemy 0x6B26D0 lee
            //      FR+0x60 como relación cuando no hay nodo; -100 <= -30 → enemigo). Cura el gate.
            //   2. fundamentalType=4 (OT_CIVILIAN) en cada "Player N" (Faction+0x34) → aggro vanilla.
            // (Nameless ya NO es necesaria: el FIX previo fallaba porque iteraba el map asumiendo
            //  un std::_Hash de MSVC siendo en realidad boost::unordered_map. Se sigue localizando
            //  Nameless solo para DIAGNÓSTICO, con el layout boost correcto, sin modificarla.)
            //
            // Se aplica UNA vez (guard atómico), en el HILO DE LÓGICA (OnGameTick), tras la carga,
            // con reintentos acotados (el FactionManager puede no estar poblado justo al cargar).
            // NO aloca ni muta contenedores (solo SET de un float + un int) → seguro y atómico.
            // ORDEN CRÍTICO: este FIX corre DESPUÉS de SetHostControlledCharTick (FIX-CONTROL), que
            // RESETEA Faction+0x78 al llamar a SetControlledChar 0x802520. Como aquí se vuelve a
            // escribir FR+0x60 cada vez hasta lograrlo, el reset queda re-cubierto en el siguiente tick.
            // EN: ── [FIX-HOSTILITY] (REDESIGNED 2026-06-19) hostile defaultRelation on "Player N" ── Root cause of the
            //     SYMMETRIC frozen co-op combat: "Player N" player factions are born with an EMPTY relation map
            //     (Faction+0x78) AND defaultFactionRelation = 0.0 → isEnemy says "not an enemy" to EVERYONE → the attack
            //     enqueue gate rejects the order; enemies "flee". FIX: 1) defaultFactionRelation (FR+0x60) = -100.0;
            //     2) fundamentalType=4 (OT_CIVILIAN) at Faction+0x34. Nameless is only located for DIAGNOSIS now.
            //     Applied ONCE (atomic guard), on the LOGIC THREAD, after loading, with bounded retries. No allocation or
            //     container mutation. CRITICAL ORDER: runs AFTER SetHostControlledCharTick (FIX-CONTROL), which RESETS
            //     Faction+0x78; FR+0x60 is re-written until it sticks, so the reset is re-covered on the next tick.
            {
                // ── [FIX-HOSTILITY-HOOK] Instalar el hook del gate de combate (0x6B2630) y resolver
                //   Nameless. Va FUERA del guard s_hostilityFixDone porque debe ejecutarse aunque el
                //   FIX del defaultRelation ya haya terminado: el hook es la cura del gate explícito
                //   (las 105 entries a 0). InstallCombatGateHookOnce y ResolveNamelessFactionOnce son
                //   idempotentes (guards atómicos internos) y baratos cuando ya están resueltos.
                // EN: ── [FIX-HOSTILITY-HOOK] Install the combat gate hook (0x6B2630) and resolve Nameless ── Runs OUTSIDE
                //     the s_hostilityFixDone guard because it must run even after the defaultRelation FIX is done: the hook
                //     cures the explicit gate (105 entries at 0). All these installers/resolvers are idempotent (internal
                //     atomic guards) and cheap once resolved: isAlly gate, isEnemy (2 COMDAT), DIAG-BEDREQ, FIX-ARMGATE
                //     (hasWorkingArm) and Nameless.
                if (m_gameLoaded) {
                    uintptr_t hookModBase = Memory::GetModuleBase();
                    if (hookModBase != 0) {
                        InstallCombatGateHookOnce(hookModBase);   // instala el detour de isAlly (una vez)
                        InstallEnemyGateHookOnce(hookModBase);    // [FIX-ENEMY-HOOK] instala isEnemy (2 COMDAT)
                        InstallBedReqDiagHookOnce(hookModBase);   // [DIAG-BEDREQ] hook observacional de req. de cama (una vez)
                        InstallArmGateHookOnce(hookModBase);      // [FIX-ARMGATE] hook de hasWorkingArm 0x644150 (una vez)
                        ResolveNamelessFactionOnce(hookModBase);  // localiza Nameless (Opción A) o marca C
                    }

                    // (b) CAMBIO CRÍTICO: refrescar el cache del host para el hook CADA tick, SIN
                    // condición. Antes solo se refrescaba dentro del bloque con tope de 600 reintentos:
                    // durante la carga OnGameTick corre a ~230 ticks/s y el tope se agotaba en ~2.6s,
                    // ANTES de que la facción del host se resolviera (~7s) -> ABORT permanente ->
                    // s_cachedHostFR=0 para siempre -> Hook_CombatGate quedaba inerte. También cubre el
                    // caso de éxito: si SetControlledChar resetea Faction+0x78 DESPUÉS de que el fix
                    // marque done, aquí el cache se re-sincroniza igualmente.
                    // EN: (b) CRITICAL CHANGE: refresh the host cache for the hook EVERY tick, unconditionally. Before it was only
                    //     refreshed inside the block capped at 600 retries: during loading OnGameTick runs at ~230 ticks/s and
                    //     the cap ran out in ~2.6 s, BEFORE the host faction resolved (~7 s) → permanent ABORT → s_cachedHostFR=0
                    //     forever → Hook_CombatGate inert. It also re-syncs the cache if SetControlledChar resets Faction+0x78
                    //     after the fix is marked done. Option B = global player faction; option A = primary char faction.
                    uintptr_t cacheFaction = game::GetPlayerFactionDirect();          // Opción B
                    if (cacheFaction == 0) {
                        void* pc = m_playerController.GetPrimaryCharacter();          // Opción A
                        if (pc != nullptr) {
                            const int facOff = game::GetOffsets().character.faction;  // +0x10
                            uintptr_t fac = 0;
                            if (Memory::Read(reinterpret_cast<uintptr_t>(pc) + facOff, fac))
                                cacheFaction = fac;
                        }
                    }
                    RefreshHostFactionCache(cacheFaction);  // no-op interno si cacheFaction==0
                }

// EN: One-shot state of the defaultRelation FIX: done flag, attempts (cap 600) and consecutive confirmed
//     applications (≥3 needed to cover the SetControlledChar/FIX-CONTROL reset).
                static std::atomic<bool> s_hostilityFixDone{false};
                static int  s_hostilityFixTries = 0;
                static int  s_hostilityConfirms = 0;   // aplicaciones confirmadas CONSECUTIVAS
                static constexpr int kMaxHostilityFixTries = 600;  // ~ varios seg de margen
                static constexpr int kHostilityConfirmsNeeded = 3; // re-aplicar ≥3 ticks (cubre reset
                                                                   // de SetControlledChar/FIX-CONTROL)
                if (!s_hostilityFixDone.load(std::memory_order_acquire) && m_gameLoaded) {
                    // (a) el incremento de s_hostilityFixTries se movió DENTRO de la rama
                    // hostFaction != 0: esperar a que la facción exista NO consume el tope.
                    // EN: (a) the s_hostilityFixTries increment was moved INSIDE the hostFaction != 0 branch: waiting for the
                    //     faction to exist does NOT consume the cap.

                    // ── Resolver la facción del HOST DIRECTAMENTE (sin iterar el FactionManager) ──
                    // Causa del ABORTO anterior: la búsqueda por isPlayerIface(+0x250)!=0 NO halla
                    // ninguna facción en este gamestart (el flag está a 0). Pero el host SÍ es
                    // player-controlled porque char.faction == playerFaction global (lo confirma
                    // [DIAG-PCTRL]). Así que tomamos esa facción y escribimos sobre ella sin buscar.
                    //   Opción B (PRIMARIA): playerFaction global = GW+0x580 -> +0x2A0
                    //   Opción A (FALLBACK): char.faction = primaryChar + character.faction(0x10)
                    // EN: ── Resolve the HOST faction DIRECTLY (without walking the FactionManager) ── Cause of the previous
                    //     ABORT: the isPlayerIface(+0x250)!=0 search finds no faction in this game start, but the host IS player
                    //     controlled because char.faction == global playerFaction ([DIAG-PCTRL]). So that faction is taken and
                    //     written without searching. Option B (PRIMARY): global playerFaction GW+0x580 -> +0x2A0; option A
                    //     (FALLBACK): primaryChar + character.faction (0x10).
                    uintptr_t hostFaction = game::GetPlayerFactionDirect();   // Opción B
                    if (hostFaction == 0) {
                        // Fallback Opción A: leer la facción desde el char primario del host.
                        void* primaryChar = m_playerController.GetPrimaryCharacter();
                        if (primaryChar != nullptr) {
                            const int facOff = game::GetOffsets().character.faction; // +0x10
                            uintptr_t fac = 0;
                            if (Memory::Read(reinterpret_cast<uintptr_t>(primaryChar) + facOff, fac))
                                hostFaction = fac;
                        }
                    }

                    if (hostFaction != 0) {
                        // ES: Resolver offsets de Faction (relations 0x78) y FactionRelations (size 0x40, defaultRelation 0x60).
                        // EN: Resolve Faction (relations 0x78) and FactionRelations (size 0x40, defaultRelation 0x60) offsets.
                        s_hostilityFixTries++;  // (a) solo cuentan intentos REALES (facción resuelta)
                        const auto& off  = game::GetOffsets().faction;
                        const auto& frel = game::GetOffsets().factionRelations;

                        // ── [FIX-HOSTILITY-HOOK] mantener al día el cache del host para el hook del
                        //   gate de combate (0x6B2630). Es barato (2 derefs) y garantiza que el detour
                        //   reconozca this==hostFR aunque SetControlledChar haya reseteado Faction+0x78.
                        // EN: ── [FIX-HOSTILITY-HOOK] keep the host cache up to date for the combat gate hook (0x6B2630). Cheap
                        //     (2 derefs) and guarantees the detour recognizes this==hostFR even if SetControlledChar reset
                        //     Faction+0x78.
                        RefreshHostFactionCache(hostFaction);

                        HostilityFixResult fr{};
                        // Escritura DIRECTA sobre la facción del host: FR+0x60=-100, fundamentalType=4.
                        // EN: DIRECT write on the host faction: FR+0x60=-100, fundamentalType=4.
                        SEH_ApplyHostilityFixDirect(hostFaction, off.relations /*0x78*/,
                                                    frel.size /*0x40*/,
                                                    frel.defaultRelation /*0x60*/, &fr);

                        if (fr.applied) {
                            // ORDEN: el FIX-HOSTILITY corre en Step 0; SetHostControlledCharTick
                            // (FIX-CONTROL) corre en Steps posteriores del MISMO tick y RESETEA
                            // Faction+0x78 al llamar a SetControlledChar. Por eso NO marcamos done al
                            // primer éxito: re-aplicamos durante ≥3 ticks consecutivos para volver a
                            // escribir el default tras el reset. SetControlledChar es one-shot (se
                            // aplica 1 vez), así que tras unos ticks el default ya persiste estable.
                            // EN: ORDER: FIX-HOSTILITY runs in Step 0; SetHostControlledCharTick (FIX-CONTROL) runs later in the SAME
                            //     tick and RESETS Faction+0x78 when calling SetControlledChar. So done is NOT set on the first success:
                            //     it is re-applied for ≥3 consecutive ticks to re-write the default after the reset (SetControlledChar
                            //     is one-shot, so after a few ticks the default stays stable).
                            s_hostilityConfirms++;
                            if (s_hostilityConfirms == 1 || s_hostilityConfirms == kHostilityConfirmsNeeded) {
                                spdlog::info(
                                    "[FIX-HOSTILITY] APLICADO ✓ faction=0x{:X} (host, vía DIRECTA) "
                                    "defaultHostil(-100)-escrito={} tipo-fijado(OT_CIVILIAN)={} "
                                    "relCount(+0x40 boost)={} | RIESGO auto-ataque-squad: si relCount>0 "
                                    "es probable que la entry propia exista y el default solo afecte a "
                                    "desconocidas (verificar que el squad NO se auto-ataca) "
                                    "(confirmación {}/{}, intento #{})",
                                    fr.namelessFaction, fr.defaultsWritten, fr.typesFixed,
                                    fr.namelessRelCount,
                                    s_hostilityConfirms, kHostilityConfirmsNeeded, s_hostilityFixTries);
                            }
                            if (s_hostilityConfirms >= kHostilityConfirmsNeeded) {
                                s_hostilityFixDone.store(true, std::memory_order_release);
                            }
                        } else {
                            s_hostilityConfirms = 0;  // racha rota (aún no aplicable) — re-confirmar
                            // FR no resoluble este tick (facción a medio inicializar). Reintentamos.
                            // EN: Streak broken (not applicable yet) — re-confirm. FR not resolvable this tick (faction half
                            //     initialized): retry.
                            if (s_hostilityFixTries <= 10 || s_hostilityFixTries % 120 == 0) {
                                spdlog::info(
                                    "[FIX-HOSTILITY] aún no aplicable (faction=0x{:X} FR(+0x78) no "
                                    "resoluble este tick) — reintento #{}",
                                    hostFaction, s_hostilityFixTries);
                            }
                        }
                    } else {
                        // Facción del host aún no resuelta (carga a medias / cadena GW+0x580 no lista).
                        // (a) NO quemamos el tope: durante la carga van ~230 ticks/s y los 600
                        // intentos se agotaban en ~2.6s, antes de los ~7s que tarda la facción.
                        // EN: Host faction not resolved yet (half-loaded / GW+0x580 chain not ready). (a) The cap is NOT burned:
                        //     during loading there are ~230 ticks/s and the 600 attempts ran out in ~2.6 s, before the ~7 s the
                        //     faction takes.
                        s_hostilityConfirms = 0;
                        static int s_hostilityWaitTicks = 0;
                        ++s_hostilityWaitTicks;
                        if (s_hostilityWaitTicks <= 10 || s_hostilityWaitTicks % 600 == 0) {
                            spdlog::info(
                                "[FIX-HOSTILITY] facción del host no resuelta aún "
                                "(GetPlayerFactionDirect=0 y char+0x10 no legible) — esperando "
                                "(tick de espera #{}, intentos reales consumidos={})",
                                s_hostilityWaitTicks, s_hostilityFixTries);
                        }
                    }

                    // Tope de seguridad: si tras muchos intentos no se pudo, dejamos de intentar.
                    // Causa probable ahora: la cadena GW+0x580->+0x2A0 no se resuelve (carga
                    // incompleta). Ya NO depende de hallar isPlayerIface!=0.
                    // EN: Safety cap: after many attempts, stop trying. Likely cause now: the GW+0x580->+0x2A0 chain does not
                    //     resolve (incomplete load). It no longer depends on finding isPlayerIface!=0.
                    if (!s_hostilityFixDone.load(std::memory_order_acquire) &&
                        s_hostilityFixTries >= kMaxHostilityFixTries) {
                        s_hostilityFixDone.store(true, std::memory_order_release); // no insistir más
                        spdlog::warn(
                            "[FIX-HOSTILITY] ABORTADO tras {} intentos: no se pudo resolver la facción "
                            "del host (GetPlayerFactionDirect GW+0x580->+0x2A0 = 0 y fallback char+0x10 "
                            "no legible). El combate seguirá neutral hasta resolverlo.",
                            s_hostilityFixTries);
                    }
                }
            }
        }
    }

    // ES: ── Step 0.5: limpiar snapshots pendientes antiguos ── Cada 300 ticks se borran las actualizaciones de
    //     posición de más de 10 s encoladas para entidades que nunca spawnearon (evita crecimiento de memoria).
    // ── Step 0.5: Cleanup old pending snapshots ──
    // Every 300 ticks (~5 seconds at 60fps), remove position updates older than 10s
    // that were queued for entities that never spawned. Prevents unbounded memory growth.
    {
        static int s_cleanupCounter = 0;
        if (++s_cleanupCounter % 300 == 0) {
            PendingSnapshotQueue::CleanupOld(SessionTime(), 10.0f);
        }
    }

    // ES: ── Step 1: escaneo de entidades con reintentos ── cada 150 ticks (~1 s) hasta 45 reintentos:
    //     primero reclamar "Player N" por nombre, luego el char primario por cadena directa y, si no, escaneo
    //     por facción. (El comentario inglés dice "30 segundos"; el límite real es 45 reintentos.)
    // EN: (The real limit is 45 retries, about 30-45 s depending on frame rate.)
    // ── Step 1: Entity scan with retry ──
    // SendExistingEntitiesToServer may fail on first attempt if CharacterIterator
    // can't resolve PlayerBase/GameWorld. Retry every 150 ticks (~1 second) for
    // up to 30 seconds until at least one character is sent.
    if (!m_initialEntityScanDone) {
        // ES: Reiniciar el contador de reintentos al empezar de cero (tras desconectar/reconectar); los estáticos
        //     son de ámbito de fichero para que HandleSpawnQueue pueda reiniciarlos.
        // Reset retry counter when starting fresh (after disconnect/reconnect).
        // Without this, the static counter persists across connections and may
        // exhaust retries prematurely on the second connection.
        // s_entityScanRetries and s_wasScanning are file-scope statics
        // (declared above OnGameTick) so HandleSpawnQueue can reset them.
        if (!s_wasScanning) {
            s_entityScanRetries = 0;
            s_wasScanning = true;
        }
        static constexpr int MAX_ENTITY_SCAN_RETRIES = 45; // ~30 seconds at 150fps
        static constexpr int RETRY_INTERVAL_TICKS = 150;

        bool shouldAttempt = (s_entityScanRetries == 0) ||
                             (s_tickCallCount % RETRY_INTERVAL_TICKS == 0);

        if (shouldAttempt) {
            if (s_entityScanRetries == 0) {
                spdlog::info("Core::OnGameTick: Step 1 — scanning for characters");
                m_nativeHud.LogStep("GAME", "Scanning for characters...");
            }

            // Try mod character claiming FIRST (finds "Player N" by name — most reliable
            // en el flujo cargar->conectar, donde el char se llama "Player N").
            // Falls back to faction-based scan if no mod characters found.
            // EN: (…in the load->connect flow, where the char is named "Player N".)
            if (m_lobbyManager.HasFaction() && m_lobbyManager.GetPlayerSlot() > 0) {
                FindAndClaimModCharacters();
            }

            // ── FIX connected-then-load: claim del char PRIMARIO por cadena directa ──
            // Si el claim por nombre "Player N" NO reclamó nada (caso típico del flujo
            // connected-then-load: el char del host conserva el nombre del save, no "Player N",
            // y el hook CharacterCreate estaba en passthrough durante la carga), reclamamos el
            // char primario del jugador por la lista REAL del motor (GW+0x580 -> +0x2B0 -> data[0]).
            // Idempotente y robusto: no depende del nombre ni de la captura del hook.
            // EN: ── FIX connected-then-load: claim the PRIMARY char through the direct chain ── If the "Player N" name
            //     claim claimed nothing (typical of the connected-then-load flow: the host char keeps the save name and
            //     the CharacterCreate hook was in passthrough), claim the player primary char through the engine REAL
            //     list (GW+0x580 -> +0x2B0 -> data[0]). Idempotent and robust.
            if (m_entityRegistry.GetPlayerEntities(m_localPlayerId).empty()) {
                ClaimHostPrimaryCharacter();
            }

            // Faction-based scan as fallback (finds squad members by faction pointer)
            if (m_entityRegistry.GetPlayerEntities(m_localPlayerId).empty()) {
                SendExistingEntitiesToServer();
            }
            auto localCount = m_entityRegistry.GetPlayerEntities(m_localPlayerId).size();
            s_entityScanRetries++;

            if (localCount > 0) {
                m_initialEntityScanDone = true;
                s_wasScanning = false; // Allow fresh retry on next connection
                m_nativeHud.LogStep("OK", "Sent " + std::to_string(localCount) + " characters to server");
                spdlog::info("Core: Entity scan succeeded on attempt {} — {} characters", s_entityScanRetries, localCount);
            } else if (s_entityScanRetries >= MAX_ENTITY_SCAN_RETRIES) {
                m_initialEntityScanDone = true;
                s_wasScanning = false; // Allow fresh retry on next connection
                m_nativeHud.LogStep("WARN", "Entity scan: no squad characters found after " +
                                    std::to_string(s_entityScanRetries) + " attempts");
                spdlog::warn("Core: Entity scan exhausted {} retries — 0 characters found", s_entityScanRetries);
            } else if (s_entityScanRetries <= 3 || s_entityScanRetries % 10 == 0) {
                spdlog::info("Core: Entity scan attempt {} found 0 chars — will retry", s_entityScanRetries);
            }
        }
    }

    // ── Step 1a': Red de seguridad — claim del char del host por cadena directa ──
    // Si el scan inicial AGOTÓ sus reintentos (m_initialEntityScanDone=true) pero el host
    // SIGUE sin ningún char local registrado, la lista del jugador pudo poblarse tarde
    // (timing del flujo connected-then-load). Reintentamos el claim directo a baja frecuencia
    // (~cada 2s) mientras estemos en juego y conectados. Sin esto, agotar los 45 reintentos
    // del Step 1 dejaría al host sin reclamar PARA SIEMPRE (entities=0 permanente).
    // Idempotente: ClaimHostPrimaryCharacter no duplica si ya está registrado.
    // EN: ── Step 1a': Safety net — host char claim through the direct chain ── If the initial scan EXHAUSTED its
    //     retries but the host STILL has no local char registered, the player list may have filled late. Retry
    //     the direct claim at low frequency (~every 2 s) while in game and connected. Without this, exhausting
    //     the 45 retries of Step 1 would leave the host unclaimed FOREVER (entities=0). Idempotent.
    if (m_gameLoaded && m_connected.load() &&
        m_entityRegistry.GetPlayerEntities(m_localPlayerId).empty()) {
        if (s_tickCallCount % 300 == 0) { // ~cada 2s a 150fps
            if (ClaimHostPrimaryCharacter()) {
                spdlog::info("Core::OnGameTick: Step 1a' — host primary char reclamado (red de seguridad post-scan)");
            }
        }
    }

    // ES: ── Step 1b: re-escaneo diferido tras el bootstrap de facción ── entity_hooks saca la facción del
    //     primer personaje que ve; después se re-escanea para registrar el escuadrón ya cargado.
    // ── Step 1b: Deferred re-scan after faction bootstrap ──
    // entity_hooks bootstraps the faction from the first character it sees.
    // After that, we re-scan to register the player's existing squad members
    // (which loaded before hooks were installed).
    if (m_needsEntityRescan.exchange(false)) {
        spdlog::info("Core::OnGameTick: Faction bootstrap triggered — re-scanning existing characters");
        m_nativeHud.LogStep("GAME", "Faction bootstrapped — re-scanning squad...");
        SendExistingEntitiesToServer();
        auto localCount = m_entityRegistry.GetPlayerEntities(m_localPlayerId).size();
        m_nativeHud.LogStep("OK", "Re-scan found " + std::to_string(localCount) + " squad characters");
        if (localCount > 0) {
            m_initialEntityScanDone = true; // Stop retrying
        }
    }
    // ES: ── Step 1c: descubrimiento diferido de SquadAddMember por vtable ── si el escaneo de patrones falló,
    //     se reintenta cada ~100 ticks (máx. 30 veces) hasta tener un escuadrón del que leer la vtable.
    // ── Step 1c: Deferred vtable discovery for SquadAddMember ──
    // If pattern scan failed and OnGameLoaded didn't find a squad yet,
    // keep trying each tick until we have a squad to read the vtable from.
    if (!m_gameFuncs.SquadAddMember && !s_squadAddMemberDiscovered) {
        static int s_vtableRetries = 0;
        if (s_vtableRetries < 30 && s_tickCallCount % 100 == 0) { // Try every ~0.7s for ~21s
            s_vtableRetries++;
            TryDiscoverSquadAddMemberFromVTable(m_gameFuncs, m_scanner.GetBase(), m_scanner.GetSize());
        }
    }

    // ES: Miga de pan del paso: la lee el VEH si hay crash.
    // EN: Step breadcrumb: read by the VEH on a crash.
    g_lastTickStep = 1; g_lastStepName = "entity_scan";
    SetLastCompletedStep(1);

    // ES: ── Step 1.5: drenar la cola de comandos ANTES del pipeline de sync ── CRÍTICO: toda escritura de
    //     memoria del juego que venga del hilo de red pasa por aquí, en el hilo de juego/OGRE. Cada comando
    //     se ejecuta con su propio SEH (SEH_RunGameCommand).
    // ── Step 1.5: Drain command queue BEFORE sync pipeline ──
    // CRITICAL: All network thread → game memory writes must go through this queue.
    // Drain on game/OGRE thread to ensure thread safety.
    {
        size_t cmdCount = m_commandQueue.Size();
        if (cmdCount > 0) {
            m_commandQueue.DrainAll([](GameCommand& cmd) {
                if (!SEH_RunGameCommand(&cmd)) {
                    spdlog::warn("Core: comando de red descartado por excepción durante ejecución");
                }
            });
            if (cmdCount > 100) {
                spdlog::debug("Core::OnGameTick: Drained {} commands from queue", cmdCount);
            }
        }
    }

    // ES: ── Steps 2-9: pipeline de sincronización ── Rama NUEVA (SyncOrchestrator: interpolación, tick del
    //     orquestador, loading orch, cola de spawns, fixes del host, sondas y eventos diferidos, SDK, proxy
    //     visual, shared save, teletransporte) o rama LEGACY de doble buffer (else). Nota: los números de
    //     SetLastCompletedStep no siempre coinciden con los de g_lastTickStep.
    // EN: ── Steps 2-9: sync pipeline ── NEW branch (SyncOrchestrator: interpolation, orchestrator tick, loading
    //     orch, spawn queue, host fixes, deferred probes/events, SDK, visual proxy, shared save, teleport) or
    //     LEGACY double-buffer branch (else). Note: SetLastCompletedStep numbers do not always match
    //     g_lastTickStep ones.
    // ── Steps 2-9: Sync pipeline ──
    if (m_useSyncOrchestrator && m_syncOrchestrator) {
        g_lastTickStep = 2; g_lastStepName = "interpolation";
        WriteBreadcrumb("interpolation", s_tickCallCount, 2);
        SEH_InterpolationUpdate(&m_interpolation, deltaTime);
        SetLastCompletedStep(4);

        g_lastTickStep = 3; g_lastStepName = "sync_orch_tick";
        WriteBreadcrumb("sync_orch_tick", s_tickCallCount, 3);
        SEH_SyncOrchestratorTick(m_syncOrchestrator.get(), deltaTime);
        SetLastCompletedStep(6);

        g_lastTickStep = 4; g_lastStepName = "loading_orch";
        WriteBreadcrumb("loading_orch", s_tickCallCount, 4);
        SEH_LoadingOrchTick(&m_loadingOrch);

        g_lastTickStep = 5; g_lastStepName = "handle_spawns";
        WriteBreadcrumb("handle_spawns_sync", s_tickCallCount, 5);
        HandleSpawnQueue();

        // ES: Procesar las sondas diferidas de AnimClass (descubren la cadena de posición física).
        // Process deferred AnimClass probes (discovers physics position chain)
        game::ProcessDeferredAnimClassProbes();

        // [DIAG] Sonda v2 de facción del jugador. Throttle interno de 2s reales y solo
        // dispara cuando el PlayerBase ya es heap válido (player existe). Máx 6 volcados.
        // EN: [DIAG] Player faction probe v2. Internal 2 s real-time throttle; only fires once PlayerBase is a valid
        //     heap pointer (player exists). Max 6 dumps.
        // ES: ── Fixes one-shot del HOST (solo si IsHost()), en este orden: facción, semillas de tiempo
        //     (+0xD0 / +0x4C0), flags de brazo, relaciones corruptas, SetControlledChar, CombatClass, arranque de
        //     combate, handle de brazo, platoon, autotest, detección y despawn del clon del squad. ──
        // EN: ── One-shot HOST fixes (only if IsHost()), in this order: faction, time seeds (+0xD0 / +0x4C0), arm
        //     flags, corrupted relations, SetControlledChar, CombatClass, combat boot, arm handle, platoon,
        //     autotest, squad clone detection and despawn. ──
        game::DiagTickPump();

        // [DIAG-FAC] Fix de facción del char del HOST: escribe la player faction válida
        // en char+0x10 si está NULL/incorrecta, para que el motor te reconozca como
        // jugador y acepte tus órdenes de combate. One-shot, throttled, solo host.
        // EN: [DIAG-FAC] HOST char faction fix: writes the valid player faction into char+0x10 if NULL/wrong, so the
        //     engine recognizes you as the player and accepts your combat orders.
        if (IsHost()) FixHostCharacterFactionTick(*this);

        // [FIX-SIMSEED] Seed de char+0xD0 (lastProcessed) del host: desbloquea el AI tick de
        // combate. Con +0xD0=0.0 y reloj>12h el AI tick (0x5CCD90) cae siempre en la rama
        // cleanup (diff>12) que NO procesa combate ni escribe +0xD0 → bucle infinito. Sembrar
        // +0xD0=reloj fuerza diff<=12 → rama combate. One-shot, throttled, solo host.
        // EN: [FIX-SIMSEED] Seed of the host char+0xD0 (lastProcessed). (See the DIAG-AICHK correction: this only
        //     affects the corpse branch.)
        if (IsHost()) SeedHostCharLastProcessedTick(*this);

        // [FIX-MEDSEED] Seed de char+0x4C0 (timestamp medico) del host: desbloquea el consumo de
        // comida. Con char+0x4C0 basura/futuro, MedicalSystem::periodicUpdate (0x64DA70) calcula
        // dt = SimClock - char+0x4C0 SIN clamp → dt<0 → el hambre SUBE y el char nunca come.
        // Sembrar char+0x4C0=SimClock fuerza dt>=0. One-shot, throttled, solo host (hermano del
        // FIX-SIMSEED, mismo mecanismo en otro offset).
        // EN: [FIX-MEDSEED] Seed of the host char+0x4C0 (medical timestamp): forces dt>=0 so hunger drops and the
        //     char eats.
        if (IsHost()) SeedHostCharMedicalTimestampTick(*this);

        // [FIX-ARMSEED] Recalcula el bool cacheado "brazo OK" (char+0x5BE) del host vía
        // reassessCollapseMode (0x649320). Sin esto, si el brazo nunca se dañó no llega ninguna
        // escritura de salud por red y el flag se queda mal → no puedes cargar a nadie a cuestas.
        // NO one-shot: throttle ~2.5s, idempotente, solo host (hermano del FIX-MEDSEED).
        // EN: [FIX-ARMSEED] Recomputes the host cached "arm OK" bool (char+0x5BE) via reassessCollapseMode
        //     (0x649320). NOT one-shot: ~2.5 s throttle, idempotent.
        if (IsHost()) SeedHostCharArmFlagsTick(*this);

        // [FIX-HOSTREL] Corrige las 37 relaciones de facción corruptas (neutral→-100) del host que
        // impedían que los NPC contraatacaran ("todos huyen"). Reutiliza s_cachedHostFR. One-shot
        // re-armable, throttled ~1s, solo host. Coexiste con el FIX-HOSTILITY-HOOK (isAlly).
        // EN: [FIX-HOSTREL] Fixes the 37 corrupted host faction relations (neutral→-100) that stopped NPCs from
        //     fighting back. Reuses s_cachedHostFR. Re-armable one-shot, ~1 s throttle.
        if (IsHost()) FixHostFactionRelationsTick(*this);

        // [FIX-CONTROL] CAUSA 2: vincular el char del host como "controlado por el jugador"
        // (SetControlledChar 0x802520 → faction+0x250=PI). El gate de la rama viva 0x5CD1E3
        // exime al host del umbral 0.75 → su think (atacar/curar/levantar/auto-defensa) corre
        // siempre, no intermitente. One-shot idempotente, throttled, solo host. Coexiste con el
        // FIX-HOSTILITY (relaciones) — son las 2 causas separadas del combate congelado.
        // EN: [FIX-CONTROL] CAUSE 2: bind the host char as "player controlled" (SetControlledChar 0x802520 →
        //     faction+0x250=PI) so its think always runs.
        if (IsHost()) SetHostControlledCharTick(*this);

        // [FIX-COMBATCLASS] CAUSA RAÍZ del combate: crear la CombatClass (*(CharBody+0x8)) del
        // host si nació NULL (el spawn del mod saltó giveBirth→CharBody::create). One-shot
        // idempotente, throttled, solo host. Detrás de toggle kEnableCombatClassFix (OFF por
        // defecto: validar primero con [DIAG-ATTACK]). Coexiste con FIX-CONTROL/FIX-HOSTILITY.
        // EN: [FIX-COMBATCLASS] Creates the host CombatClass (*(CharBody+0x8)) if born NULL. NOTE: the comment says
        //     "OFF by default" but kEnableCombatClassFix is currently true.
        if (IsHost()) FixHostCombatClassTick(*this);

        // [FIX-COMBATARM] Arranca la máquina de estados de combate del host llamando a
        // CombatClass::startupState() (vtable slot +0x50) una vez, para que nextMove(CC+0x1F4)/
        // combatState(CC+0x1F0) dejen de tener basura sin inicializar (el host podía no iniciar
        // combate en frío hasta usar un muñeco). Red de seguridad: sanea nextMove/combatState si
        // siguen fuera de rango. Va DESPUÉS de FIX-COMBATCLASS (necesita la CombatClass ya creada).
        // One-shot idempotente, throttled, solo host, hilo de lógica.
        // EN: [FIX-COMBATARM] Boots the host combat state machine (CombatClass::startupState, vtable slot +0x50) once,
        //     with the nextMove/combatState safety net. Runs AFTER FIX-COMBATCLASS.
        if (IsHost()) FixHostCombatArmTick(*this);

        // [FIX-ARMHAND] Repara el handle de brazo del AI del host: AI+0x318 = Character+0x458
        // (escritura de puntero de 8 bytes a una región que ya existe dentro del propio Character).
        // Sin esto, el chequeo GOAP nativo dice "brazo en estado pésimo" aunque los brazos estén
        // sanos → el host no puede cargar/secuestrar, hacer primeros auxilios ni autocurarse.
        // One-shot idempotente, throttled, solo host. Va DESPUÉS de FIX-COMBATCLASS/FIX-COMBATARM
        // (asume host/AI ya resueltos: AI = *(char+0x650) debe estar materializado).
        // EN: [FIX-ARMHAND] Repairs the host AI arm handle: AI+0x318 = Character+0x458 (8-byte pointer write).
        //     Runs AFTER FIX-COMBATCLASS/FIX-COMBATARM.
        if (IsHost()) FixHostArmHandTick(*this);

        // [FIX-PLATOON] Re-enlaza el ActivePlatoon del host (char+0x658) registrando AI+0x10 vía
        // setActivePlatoon(0x6213F0), que el spawn del clon del mod omite → su Tasker queda
        // huérfano y el AI tick no consume las órdenes de combate. One-shot idempotente,
        // throttled, solo host. Toggle kEnablePlatoonFix (DEFAULT true). Va ANTES del [AUTOTEST]
        // para que el ciclo de prueba mida el efecto (amIdle→0 + Task_MeleeAttack al atacar).
        // EN: [FIX-PLATOON] Re-links the host ActivePlatoon (char+0x658) registering AI+0x10 via setActivePlatoon
        //     (0x6213F0). Runs BEFORE [AUTOTEST] so the test measures its effect.
        if (IsHost()) FixHostPlatoonTick(*this);

        // [AUTOTEST] Auto-test de combate: cuando hay un NPC enemigo cercano, dispara
        // attackTarget(0x5CB0A0)(host, npc) POR CÓDIGO (sin GUI) y loguea PRE/POST para
        // confirmar si el FIX de relaciones desbloqueó el ataque. UNA vez por carga,
        // throttled, solo host. Toggle kEnableCombatAutotest (DEFAULT true). Va el ÚLTIMO,
        // tras los FIX, para que la facción/control/hostilidad ya estén aplicados al disparar.
        // EN: [AUTOTEST] Combat auto-test: fires attackTarget (0x5CB0A0)(host, npc) from code and logs PRE/POST.
        //     ONCE per load; runs LAST, after the fixes.
        if (IsHost()) CombatAutotestTick(*this);

        // [DIAG-CLONESQUAD] Detecta (solo log, NO despawnea) el CharacterHuman DUPLICADO del squad
        // del host — causa raíz del bug de la cama (GET_OUT_OF_BED reencolado sin fin). One-shot
        // re-armable, throttle ~2s, solo host. Ver wiki secc. 21.
        // EN: [DIAG-CLONESQUAD] Detects (log only) the DUPLICATED CharacterHuman in the host squad (bed bug).
        if (IsHost()) DetectHostSquadCloneTick(*this);

        // [FIX-CLONESQUAD-DESPAWN] Despawn REAL del clon detectado arriba: erase de la lektor
        // PlayerInterface PRIMERO, GameWorld::removeObject(0x799AF0) DESPUÉS. Gating por estabilidad
        // (≥3 ticks con el mismo clon), guards de seguridad completos, one-shot re-armable.
        // EN: [FIX-CLONESQUAD-DESPAWN] REAL despawn of the detected clone: PlayerInterface lektor erase FIRST,
        //     GameWorld::removeObject (0x799AF0) AFTER. Stability gating (≥3 ticks), full guards, re-armable one-shot.
        if (IsHost()) DespawnHostSquadCloneTick(*this);

        // ES: Sondeo unificado de offsets (sceneNode, isPlayerControlled, aiPackage...); se desactiva solo cuando
        //     terminan todas las sondas.
        // Run unified offset prober (discovers sceneNode, isPlayerControlled, aiPackage, etc.)
        // Only runs until all probes complete; then becomes a no-op.
        if (!game::IsProberComplete() && s_tickCallCount % 30 == 5) {
            SEH_RunOffsetProber(entity_hooks::GetEarlyPlayerFaction());
        }

        // ES: Eventos diferidos: combate (muerte/KO), personajes descubiertos por el hook de animación y zonas
        //     (este último drena un ring vacío porque world_hooks no se instala); luego SDK, proxy visual y
        //     sincronización de partida compartida.
        // Process deferred combat events (death/KO queued from hook context)
        combat_hooks::ProcessDeferredEvents();

        // Process deferred character discoveries (new chars found by animation hook)
        char_tracker_hooks::ProcessDeferredDiscovery();

        // Process deferred zone events (load/unload queued from hook context)
        world_hooks::ProcessDeferredZoneEvents();

        // SDK: poll game state and compute diffs (foundation for polling-based sync)
        m_sdk.Update();

        // Visual proxy: interpolate remote player meshes toward target positions
        m_visualProxy.Update(deltaTime);

        // Shared-save sync: discover characters by name, sync positions
        shared_save_sync::Update(deltaTime);

        // ES: Step 6: teletransporte del joiner al punto de spawn del host.
        // EN: Step 6: joiner teleport to the host spawn point.
        g_lastTickStep = 6; g_lastStepName = "host_teleport";
        WriteBreadcrumb("host_teleport", s_tickCallCount, 6);
        HandleHostTeleport();
        SetLastCompletedStep(8);
        SetLastCompletedStep(9);
    } else {
    // ES: ── Rama LEGACY (doble buffer): esperar a los workers, intercambiar buffers, interpolar, aplicar
    //     posiciones remotas, leer posiciones locales, loading orch, cola de spawns, mismos fixes del host y
    //     eventos diferidos, teletransporte y relanzar el trabajo de fondo. ──
    // EN: ── LEGACY branch (double buffer): wait for workers, swap buffers, interpolate, apply remote positions,
    //     poll local positions, loading orch, spawn queue, the same host fixes and deferred events, teleport
    //     and re-kick background work. ──
        g_lastTickStep = 2; g_lastStepName = "wait_bg_work";
        WriteBreadcrumb("wait_bg_work", s_tickCallCount, 2);
        if (m_pipelineStarted) {
            m_orchestrator.WaitForFrameWork();
        }
        SetLastCompletedStep(2);

        g_lastTickStep = 3; g_lastStepName = "swap_buffers";
        WriteBreadcrumb("swap_buffers", s_tickCallCount, 3);
        if (m_pipelineStarted) {
            // ES: WaitForFrameWork() garantiza que los workers han terminado — seguro intercambiar.
            // WaitForFrameWork() guarantees workers are done — safe to swap
            int w = m_writeBuffer.load(std::memory_order_relaxed);
            int r = m_readBuffer.load(std::memory_order_relaxed);
            m_writeBuffer.store(r, std::memory_order_release);
            m_readBuffer.store(w, std::memory_order_release);
        }
        SetLastCompletedStep(3);

        g_lastTickStep = 4; g_lastStepName = "interpolation";
        WriteBreadcrumb("interpolation_alt", s_tickCallCount, 4);
        SEH_InterpolationUpdate(&m_interpolation, deltaTime);
        SetLastCompletedStep(4);

        g_lastTickStep = 5; g_lastStepName = "apply_remote_pos";
        WriteBreadcrumb("apply_remote_pos", s_tickCallCount, 5);
        // ES: Escritura directa interpolación → personaje del juego (no depende del doble buffer).
        // Direct interpolation → game character write. Always runs (no double-buffer dependency).
        // The old ApplyRemotePositions() required BackgroundInterpolate to have filled
        // the read buffer — this direct path reads interpolation results immediately.
        ApplyRemotePositionsDirect();
        SetLastCompletedStep(5);

        g_lastTickStep = 6; g_lastStepName = "poll_local_pos";
        WriteBreadcrumb("poll_local_pos", s_tickCallCount, 6);
        PollLocalPositions();

        g_lastTickStep = 7; g_lastStepName = "send_packets";
        // ES: NOTA: SendCachedPackets() eliminado — PollLocalPositions() ya envía las actualizaciones; enviar
        //     también las cacheadas duplicaba el ancho de banda.
        // NOTE: SendCachedPackets() removed — PollLocalPositions() already
        // sends fresh per-entity position updates on the main thread.
        // The background thread's cached packetBytes is a duplicate; sending
        // both doubled outbound bandwidth.
        SetLastCompletedStep(6);

        g_lastTickStep = 8; g_lastStepName = "loading_orch";
        WriteBreadcrumb("loading_orch_alt", s_tickCallCount, 8);
        SEH_LoadingOrchTick(&m_loadingOrch);

        g_lastTickStep = 9; g_lastStepName = "handle_spawns";
        WriteBreadcrumb("handle_spawns", s_tickCallCount, 9);
        HandleSpawnQueue();

        // Process deferred AnimClass probes (discovers physics position chain)
        game::ProcessDeferredAnimClassProbes();

        // [DIAG] Sonda v2 de facción del jugador (misma lógica que la rama sync).
        game::DiagTickPump();
        // EN: (Same host fixes as the sync branch — see the comments there.)

        // [DIAG-FAC] Fix de facción del char del HOST (misma lógica que la rama sync).
        if (IsHost()) FixHostCharacterFactionTick(*this);

        // [FIX-SIMSEED] Seed de char+0xD0 del host (misma lógica que la rama sync).
        if (IsHost()) SeedHostCharLastProcessedTick(*this);

        // [FIX-MEDSEED] Seed de char+0x4C0 (timestamp medico) del host (misma lógica que la rama sync).
        if (IsHost()) SeedHostCharMedicalTimestampTick(*this);

        // [FIX-ARMSEED] Recalcula el bool "brazo OK" del host (misma lógica que la rama sync).
        if (IsHost()) SeedHostCharArmFlagsTick(*this);

        // [FIX-HOSTREL] Corrige las relaciones de facción corruptas del host (misma lógica que la rama sync).
        if (IsHost()) FixHostFactionRelationsTick(*this);

        // [FIX-CONTROL] SetControlledChar del host (misma lógica que la rama sync).
        if (IsHost()) SetHostControlledCharTick(*this);

        // [FIX-COMBATCLASS] Crear CombatClass del host si nació NULL (misma lógica que la rama sync).
        if (IsHost()) FixHostCombatClassTick(*this);

        // [FIX-COMBATARM] Arranca la máquina de estados de combate del host (startupState vtbl+0x50)
        // + red de seguridad de nextMove/combatState (misma lógica que la rama sync).
        if (IsHost()) FixHostCombatArmTick(*this);

        // [FIX-ARMHAND] Repara AI+0x318 = Character+0x458 del host (misma lógica que la rama sync).
        if (IsHost()) FixHostArmHandTick(*this);

        // [FIX-PLATOON] Re-enlace AI<->platoon del host (misma lógica que la rama sync).
        if (IsHost()) FixHostPlatoonTick(*this);

        // [AUTOTEST] Auto-test de combate (misma lógica que la rama sync).
        if (IsHost()) CombatAutotestTick(*this);

        // [DIAG-CLONESQUAD] Detección del clon del squad del host (misma lógica que la rama sync).
        if (IsHost()) DetectHostSquadCloneTick(*this);

        // [FIX-CLONESQUAD-DESPAWN] Despawn REAL del clon del squad del host (misma lógica que la rama sync).
        if (IsHost()) DespawnHostSquadCloneTick(*this);

        // Run unified offset prober (legacy path — same wrapper as sync path)
        if (!game::IsProberComplete() && s_tickCallCount % 30 == 5) {
            SEH_RunOffsetProber(entity_hooks::GetEarlyPlayerFaction());
        }

        // Process deferred combat events (death/KO queued from hook context)
        combat_hooks::ProcessDeferredEvents();

        // Process deferred character discoveries (new chars found by animation hook)
        char_tracker_hooks::ProcessDeferredDiscovery();

        // Process deferred zone events (load/unload queued from hook context)
        world_hooks::ProcessDeferredZoneEvents();

        // SDK: poll game state and compute diffs
        m_sdk.Update();

        // Visual proxy: interpolate remote player meshes
        m_visualProxy.Update(deltaTime);

        // Shared-save sync: discover characters by name, sync positions
        shared_save_sync::Update(deltaTime);
        SetLastCompletedStep(7);

        g_lastTickStep = 10; g_lastStepName = "host_teleport";
        WriteBreadcrumb("host_teleport_alt", s_tickCallCount, 10);
        HandleHostTeleport();
        SetLastCompletedStep(8);

        g_lastTickStep = 11; g_lastStepName = "kick_bg_work";
        WriteBreadcrumb("kick_bg_work", s_tickCallCount, 11);
        m_frameData[m_writeBuffer.load()].Clear();
        KickBackgroundWork();
        m_pipelineStarted = true;
        SetLastCompletedStep(9);
    }

    // ES: ── Step 9b: sondas diferidas — DESACTIVADO ──
    // EN: ── Step 9b: deferred probes — DISABLED ──
    // ── Step 9b: Deferred probes — DISABLED ──
    g_lastTickStep = 12; g_lastStepName = "probes_skipped";

    // ES: ── Step 9c: validación periódica de facciones de entidades remotas ── El juego lee faction+0x250 en
    //     CADA tick de update de personaje (game+0x927E94); si la facción de un remoto se liberó (descarga de
    //     zona) el juego peta. Cada ~50 ticks se validan y reparan los punteros de facción de los remotos.
    // ── Step 9c: Periodic faction validation for remote entities ──
    // The game reads faction+0x250 on EVERY character update tick (game+0x927E94).
    // If a remote character's faction is freed (zone unload), the game crashes.
    // Fix: periodically validate and repair faction pointers for all remote entities.
    if (s_tickCallCount % 50 == 0) { // Every ~0.5 seconds
        g_lastTickStep = 12; g_lastStepName = "faction_validate";
        auto remoteEntities = m_entityRegistry.GetRemoteEntities();
        if (!remoteEntities.empty()) {
            // ES: Facción "buena" de referencia = la del personaje primario local (char+0x10), solo si es un puntero de
            //     heap alineado y fuera del módulo; con ella se reparan los punteros de facción stale de los remotos
            //     (resultado 1 = reparado, -1 = personaje liberado → se desenlaza).
            // EN: Reference "good" faction = the local primary char one (char+0x10), only if it is an aligned heap
            //     pointer outside the module; used to repair stale remote faction pointers (result 1 = repaired,
            //     -1 = character freed → unlinked).
            void* primaryChar = m_playerController.GetPrimaryCharacter();
            uintptr_t goodFaction = 0;
            if (primaryChar) {
                uintptr_t primaryPtr = reinterpret_cast<uintptr_t>(primaryChar);
                Memory::Read(primaryPtr + 0x10, goodFaction);
                if (goodFaction < 0x10000 || goodFaction >= 0x00007FFFFFFFFFFF ||
                    (goodFaction & 0x7) != 0) {
                    goodFaction = 0;
                } else {
                    uintptr_t modBase = m_scanner.GetBase();
                    size_t modSize = m_scanner.GetSize();
                    if (goodFaction >= modBase && goodFaction < modBase + modSize) {
                        goodFaction = 0;
                    }
                }
            }
            if (goodFaction != 0) {
                uintptr_t modBase = m_scanner.GetBase();
                size_t modSize = m_scanner.GetSize();
                for (EntityID eid : remoteEntities) {
                    void* gameObj = m_entityRegistry.GetGameObject(eid);
                    if (!gameObj) continue;
                    uintptr_t charPtr = reinterpret_cast<uintptr_t>(gameObj);
                    if (charPtr < 0x10000 || charPtr >= 0x00007FFFFFFFFFFF) continue;

                    int result = SEH_ValidateEntityFaction(gameObj, goodFaction, modBase, modSize);
                    if (result == 1) {
                        static int s_factionFixCount = 0;
                        if (++s_factionFixCount <= 20) {
                            spdlog::warn("Core: Fixed stale faction for remote entity {} "
                                         "(char=0x{:X})", eid, charPtr);
                        }
                    } else if (result == -1) {
                        m_entityRegistry.SetGameObject(eid, nullptr);
                    }
                }
            }
        }
    }

    // ES: ── Step 10: diagnóstico (ticks/s, etc.) ──
    // ── Step 10: Diagnostics ──
    g_lastTickStep = 13; g_lastStepName = "diagnostics";
    WriteBreadcrumb("diagnostics", s_tickCallCount, 13);
    UpdateDiagnostics(deltaTime);
    SetLastCompletedStep(10);

    // ES: ── Step 11: depurador de pipeline replicado por red ──
    // ── Step 11: Pipeline debugger ──
    g_lastTickStep = 14; g_lastStepName = "pipeline_orch";
    WriteBreadcrumb("pipeline_orch", s_tickCallCount, 14);
    m_pipelineOrch.Tick(deltaTime);
    SetLastCompletedStep(11);

    // ES: Tick completado (última miga de pan).
    // EN: Tick completed (last breadcrumb).
    g_lastTickStep = 15; g_lastStepName = "tick_complete";
    WriteBreadcrumb("tick_complete", s_tickCallCount, 15);
}

// ES: ════ Métodos del pipeline por etapas y sus helpers SEH ════
// ════════════════════════════════════════════════════════════════════════════
// Staged Pipeline Methods
// ════════════════════════════════════════════════════════════════════════════

// ES: Validación de facción de UNA entidad remota protegida con SEH. Devuelve 0=ok, 1=reparada,
//     -1=liberada (hay que desenlazarla). Solo tipos POD dentro de __try.
//     IMPORTANTE: solo repara punteros de facción realmente stale/liberados (NULL, fuera de rango o con
//     vtable fuera del módulo). NO sobrescribe facciones válidas con la del jugador local: los remotos
//     deben estar en una facción DISTINTA para que el PvP y el robo funcionen.
// SEH-protected faction validation for a single remote entity.
// Returns: 0=ok, 1=fixed, -1=freed (should unlink).
// Only POD types inside __try — safe with MSVC structured exceptions.
//
// IMPORTANT: Only fixes truly stale/freed faction pointers. Does NOT overwrite
// valid factions with the local player's faction. Remote characters need to be
// on a DIFFERENT faction for PvP combat and theft to work. If we assign the
// local player's faction, the game treats them as allies and crashes on
// "attack unprovoked" and theft interactions.
static int SEH_ValidateEntityFaction(void* gameObj, uintptr_t goodFaction,
                                      uintptr_t modBase, size_t modSize) {
    __try {
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(gameObj);
        uintptr_t faction = 0;
        Memory::Read(charPtr + 0x10, faction);

        bool isStale = false;
        if (faction == 0 || faction < 0x10000 || faction >= 0x00007FFFFFFFFFFF ||
            (faction & 0x7) != 0) {
            isStale = true;
        } else {
            // Check if faction object's vtable is still valid (not freed)
            uintptr_t vtable = 0;
            Memory::Read(faction, vtable);
            // Vtable should point into .rdata (within the game module)
            if (vtable == 0 || vtable < modBase || vtable >= modBase + modSize) {
                isStale = true;
            }
        }
        if (isStale) {
            // Only fix if the pointer is truly invalid (freed/null/out-of-range).
            // Use goodFaction as a last resort to prevent crash on faction+0x250 read.
            Memory::Write(charPtr + 0x10, goodFaction);
            return 1;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1; // Character was freed
    }
}

// ES: Escritura de posición/rotación de una entidad protegida con SEH. WritePosition sigue cadenas de
//     punteros del motor (física, Havok...) que pueden invalidarse si el personaje se libera o está en
//     transición; sin SEH una AV aquí acaba en exit(1). Valida el cuaternión (NaN/Inf y magnitud) y no
//     escribe la rotación si el valor actual en el offset parece un puntero (SceneNode u otro de Ogre).
// SEH-protected per-entity position/rotation write.
// WritePosition follows chains of game memory pointers (physics engine, Havok, etc.)
// that may become invalid if the character is freed or mid-transition.
// Without SEH, an AV here → exit(1) → atexit crash on freed trampolines.
// CharacterAccessor and Vec3/Quat are trivially destructible (safe with __try).
static bool SEH_WritePositionRotation(void* gameObj, Vec3 pos, Quat rot) {
    __try {
        game::CharacterAccessor accessor(gameObj);
        accessor.WritePosition(pos);

        auto& offsets = game::GetOffsets().character;
        if (offsets.rotation >= 0) {
            // ES: Validar el cuaternión antes de escribir (evita el crash de Ogre en el que w=1.0 se leyó como
            //     puntero): NaN/Inf + magnitud razonable.
            // Validate quaternion before writing — prevents Ogre crash where
            // a quaternion w=1.0 (0x3F800000) was read as a pointer.
            // NaN/Inf check + magnitude sanity check.
            bool quatValid = true;
            float magSq = rot.w * rot.w + rot.x * rot.x + rot.y * rot.y + rot.z * rot.z;
            if (std::isnan(magSq) || std::isinf(magSq) || magSq < 0.5f || magSq > 1.5f) {
                quatValid = false;
            }
            if (std::isnan(rot.w) || std::isnan(rot.x) || std::isnan(rot.y) || std::isnan(rot.z) ||
                std::isinf(rot.w) || std::isinf(rot.x) || std::isinf(rot.y) || std::isinf(rot.z)) {
                quatValid = false;
            }

            if (quatValid) {
                uintptr_t charPtr = reinterpret_cast<uintptr_t>(gameObj);

                // ES: Seguridad: si el valor actual en el offset de rotación parece un puntero, NO sobrescribirlo.
                // Safety: read current value first — if the existing value at +0x58
                // looks like a pointer (>0x10000), DON'T overwrite it. It might be
                // a SceneNode* or other Ogre pointer, not a quaternion.
                uintptr_t existingVal = 0;
                Memory::Read(charPtr + offsets.rotation, existingVal);
                bool looksLikePointer = (existingVal > 0x10000 && existingVal < 0x00007FFFFFFFFFFF
                                          && (existingVal & 0x3) == 0);
                if (!looksLikePointer) {
                    Memory::Write(charPtr + offsets.rotation, rot);
                } else {
                    static int s_skipCount = 0;
                    if (++s_skipCount <= 5) {
                        spdlog::warn("SEH_WritePositionRotation: SKIPPED rotation write — "
                                     "char+0x{:X} = 0x{:X} looks like pointer, not quaternion",
                                     offsets.rotation, existingVal);
                    }
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_writeAvCount = 0;
        if (++s_writeAvCount <= 10) {
            char buf[128];
            sprintf_s(buf, "KMP: SEH_WritePositionRotation CRASHED for gameObj 0x%p\n", gameObj);
            OutputDebugStringA(buf);
        }
        return false;
    }
}

// ES: Valida la facción de un personaje recién spawneado. Solo escribe si la facción actual está STALE
//     (NULL, liberada o con vtable inválida); NO sobrescribe facciones válidas (los remotos deben estar en
//     otra facción para PvP/robo). El juego peta en game+0x927E94 si faction+0x250 no es legible.
//     Reemplazo por orden: facción de respaldo de entity_hooks → facción temprana del jugador → facción
//     local del PlayerController.
// EN: Replacement order: entity_hooks fallback faction → early player faction → PlayerController local
//     faction.
// Validate faction pointer on a spawned character. Only writes if the existing
// faction is STALE (null, freed, or invalid vtable). Does NOT overwrite valid
// factions — remote characters must stay on a different faction from the local
// player so that PvP combat ("attack unprovoked") and theft mechanics work.
// The game crashes at game+0x927E94 if faction+0x250 is unreadable.
static bool SEH_FixUpFaction_Core(void* spawnedChar) {
    __try {
        uintptr_t spawnedPtr = reinterpret_cast<uintptr_t>(spawnedChar);

        // First check: is the character's CURRENT faction valid?
        uintptr_t currentFaction = 0;
        Memory::Read(spawnedPtr + 0x10, currentFaction);

        bool currentIsValid = false;
        if (currentFaction != 0 && currentFaction >= 0x10000 &&
            currentFaction < 0x00007FFFFFFFFFFF && (currentFaction & 0x7) == 0) {
            uintptr_t vtable = 0;
            Memory::Read(currentFaction, vtable);
            uintptr_t modBase = Core::Get().GetScanner().GetBase();
            size_t modSize = Core::Get().GetScanner().GetSize();
            if (vtable != 0 && vtable >= modBase && vtable < modBase + modSize) {
                currentIsValid = true;
            }
        }

        // If current faction is valid, leave it alone. The mod template's faction
        // or the NPC's original faction is fine — it gives remote characters a
        // different allegiance so combat/theft interactions work properly.
        if (currentIsValid) {
            return false; // No fix needed
        }

        // Current faction is stale/freed — need a replacement to prevent crash.
        // Use fallback faction (any valid faction seen during character creation).
        uintptr_t faction = entity_hooks::GetFallbackFaction();

        if (faction == 0 || faction < 0x10000 || faction > 0x00007FFFFFFFFFFF) {
            faction = entity_hooks::GetEarlyPlayerFaction();
        }

        if (faction == 0 || faction < 0x10000 || faction > 0x00007FFFFFFFFFFF) {
            faction = Core::Get().GetPlayerController().GetLocalFactionPtr();
        }

        if (faction == 0 || faction < 0x10000 || faction > 0x00007FFFFFFFFFFF) {
            return false; // No valid faction available, leave as-is
        }

        // Validate the replacement faction
        uintptr_t vtable = 0;
        Memory::Read(faction, vtable);
        if (vtable == 0) return false;

        Memory::Write(spawnedPtr + 0x10, faction);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Alianza de facción para los personajes del mod, protegida con SEH: pone a 100 (aliados) la relación
//     entre la facción del remoto y la del jugador local en ambos sentidos, usando la FactionRelation
//     original del juego (vía faction_hooks) con el flag "server-sourced" para que no se reenvíe al
//     servidor. Las facciones del mod son GameData persistente (seguro, a diferencia de las de NPC → UAF).
// SEH-protected faction alliance for mod characters.
// Sets the relation between a remote character's faction and the local player's faction
// to 100 (allied). Uses the game's FactionRelation function via faction_hooks.
// Mod factions are persistent GameData — safe to use (unlike NPC factions which cause UAF).
static bool SEH_AllyModFaction(void* character) {
    __try {
        auto origFn = faction_hooks::GetOriginal();
        if (!origFn) return false;

        // Get the remote character's faction
        game::CharacterAccessor accessor(character);
        if (!accessor.IsValid()) return false;
        uintptr_t remoteFaction = accessor.GetFactionPtr();
        if (remoteFaction < 0x10000 || remoteFaction >= 0x00007FFFFFFFFFFF) return false;

        // Get local player's faction
        uintptr_t localFaction = Core::Get().GetPlayerController().GetLocalFactionPtr();
        if (localFaction == 0 || localFaction == remoteFaction) return false;

        // Set server-sourced flag to prevent feedback loop (don't send this back to server)
        faction_hooks::SetServerSourced(true);
        origFn(reinterpret_cast<void*>(localFaction), reinterpret_cast<void*>(remoteFaction), 100.0f);
        origFn(reinterpret_cast<void*>(remoteFaction), reinterpret_cast<void*>(localFaction), 100.0f);
        faction_hooks::SetServerSourced(false);

        spdlog::info("Core: Allied mod faction 0x{:X} <-> local 0x{:X}", remoteFaction, localFaction);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faction_hooks::SetServerSourced(false);
        spdlog::warn("Core: SEH_AllyModFaction crashed");
        return false;
    }
}

// ── Recalcula el flag de colapso médico tras escribir salud (duplicado literal de
// SEH_ReassessCollapse en packet_handler.cpp; static → sin colisión de enlazado). ──
// El motor cachea en MedicalSystem un BOOL "brazo OK" (char+0x458+0x166 = char+0x5BE) que
// solo refresca MedicalSystem::reassessCollapseMode. Escribir salud a pelo deja ese bool
// congelado y el gate de "cargar a cuestas" (Hook_AddOrderBackend) sigue viendo el brazo
// roto. RVA 0x649320 VERIFICADA en bytes (Steam 1.0.68; el 0x648BA0 de KenshiLib está
// obsoleto para esta build). Firma: void __fastcall(MedicalSystem* this, bool medic, bool agony).
// SOLO game thread (esta ruta corre en el procesamiento de spawn del game thread). SEH-safe.
// EN: ── Recomputes the medical collapse flag after writing health (literal duplicate of
//     SEH_ReassessCollapse in packet_handler.cpp; static → no link clash). ── The engine caches in
//     MedicalSystem an "arm OK" BOOL (char+0x458+0x166 = char+0x5BE) that only
//     MedicalSystem::reassessCollapseMode refreshes. Writing health raw leaves that bool frozen and the
//     "carry on back" gate still sees a broken arm. RVA 0x649320 VERIFIED in bytes (Steam 1.0.68; the
//     KenshiLib 0x648BA0 is obsolete for this build). Signature: void __fastcall(MedicalSystem* this,
//     bool medic, bool agony). Game thread ONLY. SEH-safe.
static void SEH_ReassessCollapse(void* character, bool clearStun) {
    __try {
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);
        if (charPtr == 0) return;
        // SALTAR si el char está MUERTO (char+0x5BC, byte isDead: 0=vivo, 1=muerto).
        // EN: SKIP if the char is DEAD (char+0x5BC, isDead byte: 0=alive, 1=dead).
        uint8_t isDead = 0;
        if (!Memory::Read(charPtr + 0x5BC, isDead) || isDead != 0) return;
        // ── FIX-STUNSEED: limpiar el "fleshStun rancio" antes del recálculo de colapso ──
        // SOLO si clearStun=true (llamado tras una escritura de salud REAL por red). El tick
        // periódico de FIX-ARMSEED (SeedHostCharArmFlagsTick) pasa clearStun=false: no hay datos
        // nuevos que justifiquen borrar stun, y hacerlo ahí anularía stun real de combate legítimo
        // del host cada 2.5s (found en revisión adversarial — el arm-seed dejaba al host inmune al
        // derribo por stun). reassessCollapseMode NO decide con flesh a secas (part+0x40): usa la
        // salud EFECTIVA = flesh(part+0x40) − fleshStun(part+0x44) (getCollapseStage RVA 0x644450
        // hace la resta). El mod sólo sincroniza flesh por red; fleshStun nunca se escribe ni se
        // simula su decaimiento, así que un valor viejo de un golpe/colapso anterior deja la salud
        // efectiva negativa y el brazo se re-marca como roto aunque la UI muestre 100%. Reseteamos
        // fleshStun=0 en TODAS las partes con el MISMO patrón de iteración que
        // SEH_WriteLimbHealthDirect (partArray@0x5F8, count@0x5F0, stride 8, fleshStun =
        // healthBase+4 = part+0x44 — ver game_types.h). Escritura LOCAL: NO toca la red ni el
        // protocolo MsgLimbHealth. OJO: sólo tocamos fleshStun(+0x44); flesh(+0x40, la salud real)
        // se deja intacto — no curamos.
        // EN: ── FIX-STUNSEED: clear the "stale fleshStun" before recomputing collapse ── ONLY if clearStun=true
        //     (called after a REAL network health write). The periodic FIX-ARMSEED tick passes false (clearing
        //     there would cancel legitimate host combat stun every 2.5 s). reassessCollapseMode uses EFFECTIVE
        //     health = flesh(part+0x40) − fleshStun(part+0x44) (getCollapseStage RVA 0x644450). The mod only syncs
        //     flesh over the network; fleshStun is never written nor decayed, so an old value leaves effective
        //     health negative and the arm is re-marked broken even though the UI shows 100%. fleshStun=0 is reset
        //     on ALL parts with the same iteration as SEH_WriteLimbHealthDirect (partArray@0x5F8, count@0x5F0,
        //     stride 8, fleshStun = part+0x44). LOCAL write only; flesh (+0x40, the real health) is untouched.
        if (clearStun) {
            auto& offsets = game::GetOffsets().character;
            if (offsets.healthPartArray >= 0 && offsets.healthBase >= 0) {
                uintptr_t partArray = 0;
                int count = 0;
                if (Memory::Read(charPtr + offsets.healthPartArray, partArray) && partArray != 0 &&
                    Memory::Read(charPtr + offsets.healthPartCount, count) && count > 0 && count <= 32) {
                    for (int i = 0; i < count; i++) {
                        uintptr_t part = 0;
                        if (!Memory::Read(partArray + i * offsets.healthStride, part) || part == 0)
                            continue;
                        // fleshStun está justo tras flesh: part+0x44 = healthBase(0x40)+4.
                        // EN: fleshStun sits right after flesh: part+0x44 = healthBase(0x40)+4.
                        Memory::Write(part + offsets.healthBase + 4, 0.0f);
                    }
                }
            }
        }
        // MedicalSystem vive INLINE dentro del Character en char+0x458 (NO es puntero).
        // EN: MedicalSystem lives INLINE inside the Character at char+0x458 (NOT a pointer). Native call to
        //     reassessCollapseMode at modBase+0x649320.
        void* medicalSystem = reinterpret_cast<void*>(charPtr + 0x458);
        using ReassessCollapseFn = void(__fastcall*)(void* medical, bool medic, bool agony);
        auto fn = reinterpret_cast<ReassessCollapseFn>(Memory::GetModuleBase() + 0x649320);
        fn(medicalSystem, false, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Una llamada nativa fallida jamás debe tumbar el game thread.
        // EN: A failed native call must never take down the game thread.
    }
}

// SEH-protected limb health write for remote character spawn/link.
// Extracted from HandleSpawnQueue because __try cannot coexist with
// std::unordered_map (C2712). Only POD types inside __try block.
// Duplicado literal de SEH_WriteLimbHealthToChar (packet_handler.cpp) por límites del
// compilador (C2712). Cadena CANÓNICA MedicalSystem — ver game_types.h:
//   partArray = [char+0x5F8] → part_i = [partArray + i*8] → flesh @ part_i+0x40
// EN: (Literal duplicate of SEH_WriteLimbHealthToChar in packet_handler.cpp because of the C2712 compiler
//     limit. CANONICAL MedicalSystem chain — see game_types.h: partArray = [char+0x5F8] → part_i =
//     [partArray + i*8] → flesh @ part_i+0x40.) Writes up to 7 parts and returns false if the chain is not
//     readable.
static bool SEH_WriteLimbHealthDirect(void* character, const float health[7]) {
    __try {
        auto& offsets = game::GetOffsets().character;
        uintptr_t charPtr = reinterpret_cast<uintptr_t>(character);
        if (offsets.healthPartArray < 0 || offsets.healthBase < 0) return false;
        uintptr_t partArray = 0;
        if (!Memory::Read(charPtr + offsets.healthPartArray, partArray) || partArray == 0)
            return false;
        int count = 0;
        if (!Memory::Read(charPtr + offsets.healthPartCount, count)) return false;
        if (count <= 0 || count > 32) return false;
        int n = (count < 7) ? count : 7;
        for (int i = 0; i < n; i++) {
            uintptr_t part = 0;
            if (!Memory::Read(partArray + i * offsets.healthStride, part) || part == 0)
                continue;
            Memory::Write(part + offsets.healthBase, health[i]);
        }
        // Refrescar el flag de colapso UNA sola vez, tras escribir TODAS las partes
        // (fuera del bucle — evita 7 llamadas nativas seguidas por char). clearStun=true: esta
        // función SÍ acaba de escribir salud real por red, el stun rancio debe limpiarse.
        // EN: Refresh the collapse flag ONCE after writing ALL parts (outside the loop — avoids 7 native calls in a
        //     row per char). clearStun=true: this function DID just write real network health, so stale stun must
        //     be cleared.
        SEH_ReassessCollapse(character, /*clearStun=*/true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Setup post-spawn protegido con SEH para el spawn directo de respaldo de HandleSpawnQueue: 1) teleport a
//     la posición deseada, 2) nombre + facción (OnRemoteCharacterSpawned), 3) marcar como controlado
//     remotamente (IA suprimida), 4) inyección en escuadrón DESACTIVADA (AV de escritura en
//     game+0xE85340 en Steam), 5) WritePlayerControlled DESACTIVADO, 6) programar la sonda diferida de
//     AnimClass. Cada paso con su propio __try; devuelve false si alguno falló.
// SEH-protected post-spawn setup for Core::HandleSpawnQueue's direct spawn fallback.
// Follows the same pattern as entity_hooks::SEH_PostSpawnSetup and
// game_tick_hooks::SEH_DirectSpawnPostSetup. All three spawn paths now have SEH.
// Only POD locals and trivially-destructible types (CharacterAccessor, Vec3) are
// created inside __try — safe with MSVC structured exception handling.
static bool SEH_FallbackPostSpawnSetup(void* character, EntityID netId,
                                        PlayerID owner, Vec3 pos) {
    bool allOk = true;

    // 1. Teleport to desired position
    __try {
        game::CharacterAccessor accessor(character);
        if (pos.x != 0.f || pos.y != 0.f || pos.z != 0.f) {
            accessor.WritePosition(pos);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: FallbackPostSpawn — WritePosition AV\n");
        allOk = false;
    }

    // 2. Set name + faction
    __try {
        Core::Get().GetPlayerController().OnRemoteCharacterSpawned(
            netId, character, owner);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: FallbackPostSpawn — OnRemoteCharacterSpawned AV\n");
        allOk = false;
    }

    // 3. Mark as remote-controlled (AI decisions overridden)
    __try {
        ai_hooks::MarkRemoteControlled(character);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: FallbackPostSpawn — MarkRemoteControlled AV\n");
        allOk = false;
    }

    // 4. Squad injection DISABLED — activePlatoon resolution picks up code-section
    //    pointers on Steam builds → WRITE AV at game+0xE85340 → cascading crash.
    //    Remote characters are visible in the world without squad injection.

    // 5. WritePlayerControlled DISABLED — not needed for visibility, and can crash
    //    if the character's internal state isn't fully initialized.

    // 6. Schedule deferred AnimClass probe
    __try {
        game::ScheduleDeferredAnimClassProbe(
            reinterpret_cast<uintptr_t>(character));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("KMP: FallbackPostSpawn — AnimClassProbe AV\n");
        allOk = false;
    }

    if (!allOk) {
        static int s_crashCount = 0;
        if (++s_crashCount <= 10) {
            char buf[256];
            sprintf_s(buf, "KMP: SEH_FallbackPostSpawnSetup partial failure for entity %u "
                      "char=0x%p (surviving steps completed)\n",
                      netId, character);
            OutputDebugStringA(buf);
        }
        spdlog::error("Core: Fallback post-spawn setup partial failure for entity {} char=0x{:X}",
                      netId, reinterpret_cast<uintptr_t>(character));
    }
    return allOk;
}

// ES: Rama legacy: aplica a los personajes remotos las posiciones/rotaciones que BackgroundInterpolate dejó
//     en el buffer de lectura (m_frameData[readBuffer]). Si la escritura falla, desenlaza el objeto del
//     juego. Avisa si hay remotos con posición pero sin objeto del juego (aún no spawneados).
// EN: Legacy branch: applies to the remote characters the positions/rotations BackgroundInterpolate left in
//     the read buffer (m_frameData[readBuffer]). If the write fails, the game object is unlinked. Warns
//     when remotes have positions but no game object (not spawned yet).
void Core::ApplyRemotePositions() {
    auto& readFrame = m_frameData[m_readBuffer.load()];
    if (!readFrame.ready) return;

    static int s_applyCount = 0;
    static int s_noObjCount = 0;
    static int s_avCount = 0;
    int applied = 0;
    int noObj = 0;
    int avErrors = 0;

    for (auto& result : readFrame.remoteResults) {
        if (!result.valid) continue;

        void* gameObj = m_entityRegistry.GetGameObject(result.netId);
        if (!gameObj) {
            noObj++;
            continue;
        }

        // ES: Escritura protegida con SEH — captura AV de objetos del juego liberados/inválidos.
        // SEH-protected write — catches AV from freed/invalid game objects
        if (SEH_WritePositionRotation(gameObj, result.position, result.rotation)) {
            // Update registry tracking (safe — our own code)
            m_entityRegistry.UpdatePosition(result.netId, result.position);
            m_entityRegistry.UpdateRotation(result.netId, result.rotation);
            applied++;
        } else {
            avErrors++;
            // Unlink the bad game object so we stop crashing every frame
            m_entityRegistry.SetGameObject(result.netId, nullptr);
        }
    }

    s_applyCount += applied;
    s_noObjCount += noObj;
    s_avCount += avErrors;

    // ES: Registrar las primeras aplicaciones y luego 1 de cada 100.
    // Log first few applications, then every 100th
    if (applied > 0 && (s_applyCount <= 5 || s_applyCount % 100 == 0)) {
        spdlog::info("Core::ApplyRemotePositions: applied {} this frame (total={}, noObj={}, avErrors={})",
                     applied, s_applyCount, s_noObjCount, s_avCount);
    }

    // ES: CRÍTICO: avisar cuando hay remotos con posición pero sin objeto del juego (SpawnCharacterDirect aún no
    //     corrió o falló); sin este aviso "los jugadores no se ven" sería imposible de diagnosticar.
    // CRITICAL: Log when we have remote entities with positions but no game objects.
    // This means SpawnCharacterDirect hasn't run yet or failed for these entities.
    // Without this warning, "players can't see each other" is impossible to diagnose.
    static int s_noObjWarnings = 0;
    static auto s_lastNoObjLog = std::chrono::steady_clock::time_point{};
    if (noObj > 0 && applied == 0) {
        auto now = std::chrono::steady_clock::now();
        auto sinceLog = std::chrono::duration_cast<std::chrono::seconds>(now - s_lastNoObjLog);
        if (s_noObjWarnings < 10 || sinceLog.count() >= 5) {
            spdlog::warn("Core::ApplyRemotePositions: {} remote entities have NO game object — "
                         "positions arriving but characters not spawned yet! "
                         "(spawns pending={}, spawnReady={}, gate={})",
                         noObj, m_spawnManager.GetPendingSpawnCount(),
                         m_spawnManager.IsReady() && m_spawnManager.HasPreCallData(),
                         m_loadingOrch.GetSpawnBlockReason());
            s_noObjWarnings++;
            s_lastNoObjLog = now;
        }
    }
}

// ES: ── Aplicación directa de posiciones remotas (sin el doble buffer) ── Lee en el hilo principal el
//     resultado de la interpolación y lo escribe en los personajes remotos cada frame (valida el puntero;
//     si la escritura falla, desenlaza el objeto).
// ── Direct remote position application (bypass double-buffer pipeline) ──
// Reads interpolation results directly on the main thread and writes them
// to game characters. This eliminates timing issues with the double-buffer
// swap and ensures remote characters are updated every frame.
void Core::ApplyRemotePositionsDirect() {
    auto remoteEntities = m_entityRegistry.GetRemoteEntities();
    if (remoteEntities.empty()) return;

    float now = SessionTime();
    static int s_directApplyTotal = 0;
    static int s_directNoObjTotal = 0;
    int applied = 0;
    int noObj = 0;

    for (EntityID remoteId : remoteEntities) {
        void* gameObj = m_entityRegistry.GetGameObject(remoteId);
        if (!gameObj) {
            noObj++;
            continue;
        }

        // Validate gameObj pointer
        uintptr_t objAddr = reinterpret_cast<uintptr_t>(gameObj);
        if (objAddr < 0x10000 || objAddr >= 0x00007FFFFFFFFFFF || (objAddr & 0x7) != 0) {
            m_entityRegistry.SetGameObject(remoteId, nullptr);
            continue;
        }

        Vec3 pos;
        Quat rot;
        uint8_t moveSpeed = 0, animState = 0;
        if (m_interpolation.GetInterpolated(remoteId, now, pos, rot, moveSpeed, animState)) {
            if (SEH_WritePositionRotation(gameObj, pos, rot)) {
                m_entityRegistry.UpdatePosition(remoteId, pos);
                m_entityRegistry.UpdateRotation(remoteId, rot);
                applied++;
            } else {
                // Bad game object — unlink so we stop crashing
                m_entityRegistry.SetGameObject(remoteId, nullptr);
            }
        }
    }

    s_directApplyTotal += applied;
    s_directNoObjTotal += noObj;

    if (applied > 0 && (s_directApplyTotal <= 5 || s_directApplyTotal % 200 == 0)) {
        spdlog::info("Core::ApplyRemotePositionsDirect: applied {} this frame (total={}, noObj={})",
                     applied, s_directApplyTotal, s_directNoObjTotal);
    }

    // ES: Avisar cuando llegan posiciones pero los personajes aún no están spawneados.
    // Warn when positions are arriving but characters aren't spawned
    static int s_noObjWarnings = 0;
    static auto s_lastNoObjLog = std::chrono::steady_clock::time_point{};
    if (noObj > 0 && applied == 0) {
        auto nowClock = std::chrono::steady_clock::now();
        auto sinceLog = std::chrono::duration_cast<std::chrono::seconds>(nowClock - s_lastNoObjLog);
        if (s_noObjWarnings < 10 || sinceLog.count() >= 5) {
            spdlog::warn("Core::ApplyRemotePositionsDirect: {} remote entities have NO game object — "
                         "spawns pending={}, factory={}",
                         noObj, m_spawnManager.GetPendingSpawnCount(),
                         m_spawnManager.IsReady());
            s_noObjWarnings++;
            s_lastNoObjLog = nowClock;
        }
    }
}

// ES: Lectura de posición/rotación de un personaje protegida con SEH. Devuelve false si peta (objeto
//     liberado, cadena de punteros inválida).
// SEH-protected position read from a game character object.
// Returns false if the read crashes (freed object, invalid pointer chain).
static bool SEH_ReadPosition(void* gameObj, Vec3& outPos, Quat& outRot) {
    __try {
        game::CharacterAccessor accessor(gameObj);
        outPos = accessor.GetPosition();
        outRot = accessor.GetRotation();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ES: Lee la posición/rotación de TUS personajes locales (a la cadencia de KMP_TICK_INTERVAL_MS) y, si se
//     movieron más de KMP_POS_CHANGE_THRESHOLD, envía C2S_PositionUpdate no fiable con estado de animación
//     (0 quieto, 1 andando, 2 corriendo), velocidad comprimida a u8 y flags (0x01 = corriendo) derivados
//     del delta. Desenlaza objetos inválidos o liberados.
// EN: Reads the position/rotation of YOUR local characters (at the KMP_TICK_INTERVAL_MS rate) and, if they
//     moved more than KMP_POS_CHANGE_THRESHOLD, sends an unreliable C2S_PositionUpdate with animation state
//     (0 idle, 1 walking, 2 running), speed compressed to u8 and flags (0x01 = running) derived from the
//     delta. Unlinks invalid or freed objects.
void Core::PollLocalPositions() {
    if (!m_connected || m_localPlayerId == 0) return;

    // ES: Limitar a la cadencia de tick.
    // Throttle to tick rate
    static auto s_lastPollTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastPollTime);
    if (elapsed.count() < KMP_TICK_INTERVAL_MS) return;
    s_lastPollTime = now;

    auto localEntities = m_entityRegistry.GetPlayerEntities(m_localPlayerId);
    if (localEntities.empty()) return;

    static int s_pollsSent = 0;

    for (EntityID netId : localEntities) {
        // ES: GetInfo() devuelve por valor (std::optional) — thread-safe, sin punteros colgantes.
        // GetInfo() returns by value (std::optional<EntityInfo>) — thread-safe, no dangling pointer risk
        auto infoCopy = m_entityRegistry.GetInfo(netId);
        if (!infoCopy) continue;

        void* gameObj = m_entityRegistry.GetGameObject(netId);
        if (!gameObj) continue;

        // ES: Validar el puntero del objeto: alineado y en rango de espacio de usuario razonable.
        // Validate gameObj pointer: must be aligned and in reasonable user-space range
        uintptr_t objAddr = reinterpret_cast<uintptr_t>(gameObj);
        if (objAddr < 0x10000 || objAddr >= 0x00007FFFFFFFFFFF || (objAddr & 0x7) != 0) {
            m_entityRegistry.SetGameObject(netId, nullptr);
            spdlog::warn("PollLocalPositions: Invalid gameObj 0x{:X} for entity {} — unlinked", objAddr, netId);
            continue;
        }

        Vec3 pos;
        Quat rotation;
        if (!SEH_ReadPosition(gameObj, pos, rotation)) {
            // Object freed or invalid — unlink
            m_entityRegistry.SetGameObject(netId, nullptr);
            continue;
        }

        // ES: Saltar si la posición no cambió lo suficiente; si cambió, calcular velocidad y estado de animación.
        // Skip if position hasn't changed enough
        if (pos.DistanceTo(infoCopy->lastPosition) < KMP_POS_CHANGE_THRESHOLD) continue;

        // Compute move speed from position delta
        float elapsedSec = elapsed.count() / 1000.f;
        float dist = pos.DistanceTo(infoCopy->lastPosition);
        float moveSpeed = (elapsedSec > 0.001f) ? dist / elapsedSec : 0.f;

        uint32_t compQuat = rotation.Compress();

        // Derive animation state from speed
        uint8_t animState = 0;
        if (moveSpeed > 5.0f) animState = 2; // running
        else if (moveSpeed > 0.5f) animState = 1; // walking

        uint8_t moveSpeedU8 = static_cast<uint8_t>(
            std::min(255.f, moveSpeed / 15.f * 255.f));

        uint16_t flags = 0;
        if (moveSpeed > 3.0f) flags |= 0x01; // running

        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_PositionUpdate);
        writer.WriteU8(1);
        writer.WriteU32(netId);
        writer.WriteF32(pos.x);
        writer.WriteF32(pos.y);
        writer.WriteF32(pos.z);
        writer.WriteU32(compQuat);
        writer.WriteU8(animState);
        writer.WriteU8(moveSpeedU8);
        writer.WriteU16(flags);

        m_client.SendUnreliable(writer.Data(), writer.Size());

        m_entityRegistry.UpdatePosition(netId, pos);
        m_entityRegistry.UpdateRotation(netId, rotation);

        s_pollsSent++;
        if (s_pollsSent <= 20 || s_pollsSent % 200 == 0) {
            spdlog::debug("Core::PollLocalPositions: sent #{} netId={} pos=({:.1f},{:.1f},{:.1f}) speed={:.1f}",
                          s_pollsSent, netId, pos.x, pos.y, pos.z, moveSpeed);
        }
    }
}

// ES: Rama legacy (actualmente sin uso: ver NOTA en OnGameTick): enviaba los paquetes de posición que el
//     worker dejó cacheados en el buffer de lectura.
// EN: Legacy branch (currently unused: see the NOTE in OnGameTick): sent the position packets the worker
//     left cached in the read buffer.
void Core::SendCachedPackets() {
    auto& readFrame = m_frameData[m_readBuffer.load()];
    if (!readFrame.ready || readFrame.packetBytes.empty()) return;

    m_client.SendUnreliable(readFrame.packetBytes.data(), readFrame.packetBytes.size());
}

// ES: Solo en el joiner (no host): al recibir el punto de spawn del host espera 2 s y teletransporta el
//     escuadrón local junto a él (rejilla de 3 m, 4 por fila). One-shot por conexión (m_spawnTeleportDone).
// EN: Joiner only (not host): once the host spawn point arrives, waits 2 s and teleports the local squad
//     next to it (3 m grid, 4 per row). One-shot per connection (m_spawnTeleportDone).
void Core::HandleHostTeleport() {
    if (!HasHostSpawnPoint() || m_spawnTeleportDone || m_isHost) return;

    // ES: Copia del punto de spawn del host (protegida con mutex).
    // Snapshot host spawn point (mutex-protected)
    Vec3 hostPos = GetHostSpawnPoint();

    if (!m_hostTpTimerStarted) {
        m_hostTpTimerStarted = true;
        m_hostTpTimer = std::chrono::steady_clock::now();
        spdlog::info("Core: Host spawn point received ({:.1f}, {:.1f}, {:.1f}), "
                     "waiting 2s before teleport",
                     hostPos.x, hostPos.y, hostPos.z);
        m_nativeHud.LogStep("GAME", "Host position received, teleporting in 2s...");
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_hostTpTimer);

    if (elapsed.count() < 2) return;

    auto localEntities = m_entityRegistry.GetPlayerEntities(m_localPlayerId);
    if (localEntities.empty()) {
        spdlog::debug("Core: Waiting for local entities to register before teleporting...");
        return;
    }

    int teleported = 0;
    for (EntityID netId : localEntities) {
        void* gameObj = m_entityRegistry.GetGameObject(netId);
        if (!gameObj) continue;

        game::CharacterAccessor accessor(gameObj);
        if (!accessor.IsValid()) continue;

        Vec3 spawnPos = hostPos;
        spawnPos.x += static_cast<float>(teleported % 4) * 3.0f;
        spawnPos.z += static_cast<float>(teleported / 4) * 3.0f;

        if (accessor.WritePosition(spawnPos)) {
            m_entityRegistry.UpdatePosition(netId, spawnPos);
            teleported++;
        }
    }

    if (teleported > 0) {
        m_spawnTeleportDone = true;
        spdlog::info("Core: Teleported {} local characters to host at ({:.1f}, {:.1f}, {:.1f})",
                     teleported, hostPos.x, hostPos.y, hostPos.z);
        m_overlay.AddSystemMessage("Teleported to host location!");
        m_nativeHud.AddSystemMessage("Teleported " + std::to_string(teleported)
                                     + " characters to host!");
        m_nativeHud.LogStep("OK", "Teleported to host location!");
    }
}

// ES: Materializa a los jugadores remotos pendientes en la cola de spawns. Gating: partida cargada y
//     AssetFacilitator::CanSpawn() (con bypass de playtest tras 8 s bloqueado o /forcespawn). Hace el
//     escaneo del heap de GameData (reintentos ≤5, cada 5 s) para encontrar las plantillas del mod y, tras
//     2 s pendiente (1 intento por segundo, 1 petición por tick, máx. 1 personaje por jugador):
//     PATH 0 enlazar un "Player N" existente, PATH 1 plantilla del mod (apariencia correcta), PATH 2
//     createRandomChar (apariencia incorrecta pero funcional). También reinicia sus estáticos y re-arma
//     los fixes del host al reconectar, y limpia entidades atascadas.
// EN: Materializes the pending remote players of the spawn queue. Gating: game loaded and
//     AssetFacilitator::CanSpawn() (with a playtest bypass after 8 s blocked or /forcespawn). Runs the
//     GameData heap scan (≤5 retries, every 5 s) to find the mod templates and, after 2 s pending (1 attempt
//     per second, 1 request per tick, max 1 character per player): PATH 0 link an existing "Player N",
//     PATH 1 mod template (correct appearance), PATH 2 createRandomChar (wrong appearance but functional).
//     It also resets its statics and re-arms the host fixes on reconnect, and cleans up stuck entities.
void Core::HandleSpawnQueue() {
    // Safety: don't process spawns until game world is fully loaded AND the
    // LoadingOrchestrator says it's safe (burst ended + cooldown elapsed + no
    // pending resources). This replaces the old 20-second fixed grace period
    // with resource-aware gating via AssetFacilitator.
    if (!m_gameLoaded) return;
    if (!AssetFacilitator::Get().CanSpawn() && !m_forceSpawnBypass.load()) {
        // ES: Registrar el motivo del gate (como mucho cada 5 s) mientras haya spawns pendientes.
        // Log gate reason once per second when spawns are pending
        static auto s_lastGateLog = std::chrono::steady_clock::time_point{};
        static auto s_gateBlockStart = std::chrono::steady_clock::time_point{};
        static bool s_gateLoggedOnce = false;
        static bool s_gateTimerStarted = false;
        size_t queueSize = m_spawnManager.GetPendingSpawnCount();
        if (queueSize > 0) {
            auto now = std::chrono::steady_clock::now();
            if (!s_gateTimerStarted) {
                s_gateBlockStart = now;
                s_gateTimerStarted = true;
            }
            auto sinceLastLog = std::chrono::duration_cast<std::chrono::seconds>(now - s_lastGateLog);
            auto gateBlockDuration = std::chrono::duration_cast<std::chrono::seconds>(now - s_gateBlockStart);
            if (!s_gateLoggedOnce || sinceLastLog.count() >= 5) {
                std::string reason = m_loadingOrch.GetSpawnBlockReason();
                spdlog::warn("Core: HandleSpawnQueue BLOCKED — {} pending, gate: {} (blocked {}s)",
                             queueSize, reason, gateBlockDuration.count());
                m_nativeHud.LogStep("SPAWN", "Gate blocked: " + reason);
                s_lastGateLog = now;
                s_gateLoggedOnce = true;
            }
            // ES: BYPASS DE PLAYTEST: tras 8 s bloqueado se fuerza el paso (la partida ya cargó; el gate es demasiado
            //     conservador).
            // PLAYTEST BYPASS: After 8 seconds of blocking, force through.
            // The game is loaded — the gate is being too conservative.
            if (gateBlockDuration.count() >= 8) {
                spdlog::warn("Core: FORCE BYPASS — spawn gate blocked for {}s, forcing spawn through!",
                             gateBlockDuration.count());
                m_nativeHud.AddSystemMessage("Spawn gate bypassed (timeout) — forcing spawn...");
                s_gateTimerStarted = false;
                // Fall through to spawn logic below
            } else {
                return;
            }
        } else {
            s_gateTimerStarted = false;
        }
        if (queueSize == 0) return;
    }

    // ES: Estáticos de la cola de spawns; se reinician al reconectar (flag puesto por SetConnected(false)),
    //     junto con el re-armado de todos los fixes one-shot del host.
    // Reset statics on reconnect (flag set by SetConnected(false))
    static bool heapScanned = false;
    static int heapScanAttempts = 0;
    static auto s_lastHeapScan = std::chrono::steady_clock::time_point{};
    static auto s_lastSpawnLog = std::chrono::steady_clock::now();
    static auto s_firstPendingTime = std::chrono::steady_clock::time_point{};
    static bool s_hasPendingTimer = false;
    static int s_directSpawnAttempts = 0;
    static bool s_shownWaitingMsg = false;
    static bool s_shownTimeoutMsg = false;
    static bool s_retriedHookEnable = false;
    static int64_t s_lastNotReadyLog = 0;
    static auto s_lastDirectAttempt = std::chrono::steady_clock::time_point{};
    static std::unordered_map<PlayerID, int> s_directSpawnsPerPlayer;

    if (m_needSpawnQueueReset) {
        m_needSpawnQueueReset = false;
        heapScanned = false;
        heapScanAttempts = 0;
        s_lastHeapScan = std::chrono::steady_clock::time_point{};
        s_hasPendingTimer = false;
        s_directSpawnAttempts = 0;
        s_shownWaitingMsg = false;
        s_shownTimeoutMsg = false;
        s_retriedHookEnable = false;
        s_lastNotReadyLog = 0;
        s_wasScanning = false;
        s_entityScanRetries = 0;
        s_lastDirectAttempt = std::chrono::steady_clock::time_point{};
        s_directSpawnsPerPlayer.clear();
        ResetHostFactionFix(); // re-armar el fix de facción del host en nueva conexión
        ResetNamelessResolve(); // purgar cachés de Nameless por simetría (idempotente, barato)
        ResetHostFactionRelationsFix(); // re-armar el FIX-HOSTREL (relaciones corruptas) en nueva conexión
        ResetHostSimSeedFix(); // re-armar el seed de char+0xD0 (AI tick combate) en nueva conexión
        ResetHostMedSeedFix(); // re-armar el seed de char+0x4C0 (timestamp medico/comida) en nueva conexión
        ResetHostControlledCharFix(); // re-armar el FIX-CONTROL (SetControlledChar) en nueva conexión
        ResetHostCombatClassFix();    // re-armar el FIX-COMBATCLASS (CombatClass del host) en nueva conexión
        ResetHostCombatArmFix();      // re-armar el FIX-COMBATARM (arranque de la máquina de combate del host) en nueva conexión
        ResetHostArmHandFix();        // re-armar el FIX-ARMHAND (handle de brazo AI+0x318=Character+0x458) en nueva conexión
        ResetHostPlatoonFix();        // re-armar el FIX-PLATOON (re-enlace AI<->platoon) en nueva conexión
        ResetHostCombatAutotest();    // re-armar el [AUTOTEST] de combate en nueva conexión
        ResetHostSquadCloneDetect();  // re-armar el [DIAG-CLONESQUAD] (detección del clon del squad) en nueva conexión
        ResetHostSquadCloneDespawn(); // re-armar el [FIX-CLONESQUAD-DESPAWN] (despawn real del clon) en nueva conexión
        spdlog::info("Core::HandleSpawnQueue: Reset statics for new connection");
    }

    // ES: Escaneo del heap: corre una vez y se reintenta (≤5, enfriamiento 5 s) si no se encontraron
    //     plantillas del mod (el primer escaneo puede fallar si aún no se capturó el GameDataManager).
    // Heap scan — runs once, then retries if mod templates weren't found.
    // The first scan may fail if GameDataManager wasn't captured yet (hook was
    // disabled during loading). After ResumeForNetwork + first NPC creation,
    // m_managerPointer gets set, enabling a successful re-scan.
    bool needsScan = !heapScanned ||
        (heapScanned && m_spawnManager.GetModTemplateCount() == 0 && heapScanAttempts < 5);
    // Cooldown: don't re-scan more than once every 5 seconds
    auto timeSinceLastScan = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - s_lastHeapScan);

    // ── FIX #2 (2026-06-20): DESACOPLAR el heap scan de IsReady() ──
    // El gate previo `&& m_spawnManager.IsReady()` bloqueaba ScanGameDataHeap cuando no había
    // factory (caso: conectar con la partida ya cargada -> hook no dispara -> IsReady()=false).
    // Pero ScanGameDataHeap NO necesita el factory: su Strategy 2 deriva el GameDataManager de
    // GameWorld+0x20 (spawn_manager.cpp ~536-570). Sin escanear, m_modCandidates queda vacío,
    // FindModTemplates no encuentra los 'Player N' y modTemplates=0 -> '[loading]' eterno.
    // Permitimos el scan cuando: hay factory (IsReady), O hay un GameWorld válido (basta para
    // llenar m_modCandidates y que FindModTemplates encuentre los templates).
    // EN: ── FIX #2 (2026-06-20): DECOUPLE the heap scan from IsReady() ── The old `&& IsReady()` gate blocked
    //     ScanGameDataHeap when there was no factory (connecting to an already loaded game → hook does not fire
    //     → IsReady()=false). But ScanGameDataHeap does NOT need the factory: its Strategy 2 derives the
    //     GameDataManager from GameWorld+0x20. Without scanning, m_modCandidates stays empty, FindModTemplates
    //     finds no 'Player N' and modTemplates=0 → '[loading]' forever. The scan is allowed when there is a
    //     factory (IsReady) OR a valid GameWorld.
    bool gwValidForScan = false;
    if (m_gameFuncs.GameWorldSingleton != 0) {
        uintptr_t sb = m_scanner.GetBase();
        size_t    ss = m_scanner.GetSize();
        gwValidForScan = ValidateGameWorldGlobal(m_gameFuncs.GameWorldSingleton, sb, ss);
    }
    // Red de seguridad FIX-FACTORY-GW #1: si OnGameLoaded corrió antes de que el GameWorld
    // estuviera resuelto, reintentamos la captura del factory aquí (idempotente: no-op si ya
    // hay factory). Así IsReady() se activa en cuanto el GameWorld es válido.
    // [Oleada A 2026-07-12] Guard de intervalo añadido: sin él esta llamada corría CADA tick
    // (~160/s) mientras no hubiera factory, inundando el log con los warns de isPlausibleFactory
    // (2M de rechazos en la sesión de investigación). Mismo ritmo que FIX-FACTORY-RETRY (~1s).
    // EN: FIX-FACTORY-GW #1 safety net: if OnGameLoaded ran before the GameWorld was resolved, retry the factory
    //     capture here (idempotent). [Wave A 2026-07-12] Interval guard added: without it this ran EVERY tick
    //     (~160/s) while there was no factory, flooding the log with isPlausibleFactory warnings. Same rate as
    //     FIX-FACTORY-RETRY (~1 s).
    if (!m_spawnManager.IsReady() && gwValidForScan) {
        static int s_factoryScanRetryGuard = 0;
        if (++s_factoryScanRetryGuard % 150 == 0) {
            m_spawnManager.CaptureFactoryFromGameWorld(m_gameFuncs.GameWorldSingleton);
        }
    }
    bool canScan = m_spawnManager.IsReady() || gwValidForScan;
    if (needsScan && canScan && timeSinceLastScan.count() >= 5) {
        if (m_spawnManager.GetManagerPointer() != 0 || m_spawnManager.GetTemplateCount() < 10) {
            heapScanAttempts++;
            s_lastHeapScan = std::chrono::steady_clock::now();
            spdlog::info("Core: Triggering GameData heap scan #{} (manager=0x{:X}, templates={})...",
                         heapScanAttempts, m_spawnManager.GetManagerPointer(),
                         m_spawnManager.GetTemplateCount());
            SEH_ScanGameDataHeap(m_spawnManager);
            m_spawnManager.FindModTemplates();
            heapScanned = true;
            spdlog::info("Core: Heap scan #{} complete, {} templates available ({} mod templates)",
                         heapScanAttempts, m_spawnManager.GetTemplateCount(),
                         m_spawnManager.GetModTemplateCount());
        }
    }

    // ES: Lógica de respaldo de la cola de spawns: temporizador desde el primer spawn pendiente.
    // Spawn queue fallback logic
    size_t pending = m_spawnManager.GetPendingSpawnCount();

    if (pending > 0 && !s_hasPendingTimer) {
        s_firstPendingTime = std::chrono::steady_clock::now();
        s_hasPendingTimer = true;
        s_shownWaitingMsg = false;
        s_shownTimeoutMsg = false;
        spdlog::info("Core: {} spawn(s) queued — spawning remote player(s)...", pending);
        m_nativeHud.LogStep("GAME", std::to_string(pending) + " spawn(s) queued");
        m_nativeHud.AddSystemMessage("Spawning remote player...");
    } else if (pending == 0) {
        s_hasPendingTimer = false;
        s_directSpawnAttempts = 0;
        s_shownWaitingMsg = false;
        s_shownTimeoutMsg = false;
    }

    // ES: Mensajes de estado periódicos mientras se espera (aviso a los 10 s y timeout a los 30 s).
    // Periodic status updates while waiting
    if (pending > 0 && s_hasPendingTimer) {
        auto pendingDuration = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - s_firstPendingTime);

        // At 10s, show helpful message (direct spawn should work by 2s)
        if (pendingDuration.count() >= 10 && !s_shownWaitingMsg) {
            s_shownWaitingMsg = true;
            m_nativeHud.AddSystemMessage("Spawning taking longer than expected...");
            m_nativeHud.LogStep("GAME", "Spawn delayed 10s+");
        }

        // At 30s, show timeout warning
        if (pendingDuration.count() >= 30 && !s_shownTimeoutMsg) {
            s_shownTimeoutMsg = true;
            spdlog::warn("Core: Spawn queue waiting 30s+ — check kenshi-online.mod is in load order.");
            m_nativeHud.AddSystemMessage("Spawn timeout! Ensure kenshi-online.mod is loaded.");
            m_nativeHud.LogStep("WARN", "Spawn timeout (30s) — check mod load order");
        }
    }

    auto sinceLog = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - s_lastSpawnLog);
    if (sinceLog.count() >= 5 && pending > 0) {
        s_lastSpawnLog = std::chrono::steady_clock::now();
        spdlog::info("Core::HandleSpawnQueue: {} pending spawns (inPlaceCount={}, charTemplates={}, "
                     "factoryReady={}, hasPreCall={})",
                     pending, entity_hooks::GetInPlaceSpawnCount(),
                     m_spawnManager.GetCharacterTemplateCount(),
                     m_spawnManager.IsReady(),
                     m_spawnManager.HasPreCallData());
    }

    // ES: ═══ SPAWN DIRECTO (plantilla del mod + respaldo inmediato createRandomChar) ═══ Se intenta la
    //     plantilla del mod en cuanto se puede (2 s para el escaneo del heap); si falla, createRandomChar al
    //     momento (apariencia incorrecta pero funcional). Ya no hace falta acercarse a NPCs.
    // ═══ DIRECT SPAWN (mod template + createRandomChar immediate fallback) ═══
    // Try mod template spawn ASAP (2s for heap scan). If that fails, immediately
    // try createRandomChar as fallback (wrong appearance but functional).
    // No more "walk near NPCs" — direct spawn is the primary path.
    if (pending > 0 && s_hasPendingTimer) {
        auto pendingDuration = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - s_firstPendingTime);

        bool hasModTemplates = m_spawnManager.GetModTemplateCount() > 0;
        bool hasFactory = m_spawnManager.IsReady(); // factory captured

        // ES: Spawn directo tras 2 s. CallFactoryCreate usa RootObjectFactory::create, que construye una petición
        //     nueva desde el GameData del mod (sin punteros stale).
        // Try direct spawn after 2s (gives heap scan time to find mod templates).
        // CallFactoryCreate uses RootObjectFactory::create which builds a fresh
        // request struct from the mod GameData — no stale pointers.
        auto timeSinceLastAttempt = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - s_lastDirectAttempt);
        if (hasFactory && pendingDuration.count() >= 2
            && timeSinceLastAttempt.count() >= 1) {
            s_lastDirectAttempt = std::chrono::steady_clock::now();
            SpawnRequest spawnReq;
            if (m_spawnManager.PopNextSpawn(spawnReq)) {
                // ES: ── Salida temprana si ya está enlazado ── OnGameLoaded o HandleEntitySpawn pudieron enlazar ya esta
                //     entidad a un personaje del mod: se descarta la petición obsoleta.
                // ── Already-linked early exit ──
                // OnGameLoaded or a prior HandleEntitySpawn may have already linked this
                // entity to a mod character. If so, discard the stale spawn request.
                if (m_entityRegistry.GetGameObject(spawnReq.netId) != nullptr) {
                    spdlog::debug("Core: Entity {} already has game object — discarding stale spawn request",
                                  spawnReq.netId);
                    // Increment cap so further requests for this player are also skipped
                    s_directSpawnsPerPlayer[spawnReq.owner]++;
                    return; // Process one request per tick
                }

                // ES: ── Límite de spawns por jugador ── Solo 1 personaje por jugador remoto (evita inundar el panel de
                //     escuadrón); el resto del escuadrón queda como "fantasma" solo en el registro (posición sincronizada,
                //     sin objeto del juego).
                // ── Per-player spawn cap ──
                // Only spawn 1 character per remote player to prevent squad panel flooding.
                // The remote player's primary character is sufficient for co-op gameplay.
                // Additional squad members are tracked as registry-only ghosts (position synced
                // but no game object) to keep the world clean.
                static constexpr int MAX_DIRECT_SPAWNS_PER_PLAYER = 1;
                if (s_directSpawnsPerPlayer[spawnReq.owner] >= MAX_DIRECT_SPAWNS_PER_PLAYER) {
                    // Already spawned enough for this player — drop remaining requests silently.
                    // The entity stays in the registry (position tracked) but has no game object.
                    spdlog::debug("Core: Skipping spawn for entity {} — player {} already has {} char(s)",
                                  spawnReq.netId, spawnReq.owner, s_directSpawnsPerPlayer[spawnReq.owner]);
                    // Don't requeue — just let it go
                } else {

                // ES: ── PATH 0: enlazar un personaje del mod ya existente (en el hilo de juego CharacterIterator es fiable) ──
                //     HandleEntitySpawn pudo no encontrarlo por timing del hilo de red; aquí se reintenta. Se enlaza si está
                //     libre o ya ligado a esta misma entidad (idempotente), y se aplica setup post-spawn, nombre de GameData,
                //     salud extendida, proxy visual y alianza de facción.
                // ── PATH 0: Link existing mod character (game thread — CharacterIterator reliable) ──
                // HandleEntitySpawn may have missed the mod character (network thread timing).
                // Retry here on the game thread where CharacterIterator is guaranteed to work.
                void* existingModChar = FindModCharacterBySlot(static_cast<int>(spawnReq.owner));
                EntityID existingLinkedId = existingModChar
                    ? m_entityRegistry.GetNetId(existingModChar) : INVALID_ENTITY;
                // Link if: (a) found and unlinked, OR (b) found and linked to this same entity (idempotent)
                bool canLink = existingModChar &&
                    (existingLinkedId == INVALID_ENTITY || existingLinkedId == spawnReq.netId);
                if (canLink) {
                    spdlog::info("Core: DEFERRED MOD LINK for entity {} owner={} (game thread retry)",
                                 spawnReq.netId, spawnReq.owner);
                    m_entityRegistry.SetGameObject(spawnReq.netId, existingModChar);
                    m_entityRegistry.UpdatePosition(spawnReq.netId, spawnReq.position);
                    ai_hooks::MarkRemoteControlled(existingModChar);
                    SEH_FallbackPostSpawnSetup(existingModChar, spawnReq.netId, spawnReq.owner, spawnReq.position);
                    // Safe to write GameData template name — mod characters have unique per-slot templates
                    m_playerController.WriteGameDataNameForModLink(existingModChar, spawnReq.owner);

                    // Apply extended health
                    if (spawnReq.hasExtendedState) {
                        m_entityRegistry.UpdateLimbHealth(spawnReq.netId, spawnReq.health);
                        SEH_WriteLimbHealthDirect(existingModChar, spawnReq.health);
                    }

                    // VisualProxy
                    {
                        auto* rp = m_playerController.GetRemotePlayer(spawnReq.owner);
                        std::string dn = rp ? rp->playerName : ("Player_" + std::to_string(spawnReq.owner));
                        m_visualProxy.CreateProxy(spawnReq.netId, spawnReq.owner, dn, "", spawnReq.position, spawnReq.rotation);
                    }

                    // Faction alliance (fix 4 — applied here too)
                    SEH_AllyModFaction(existingModChar);

                    s_directSpawnAttempts++;
                    s_directSpawnsPerPlayer[spawnReq.owner]++;
                    m_nativeHud.AddSystemMessage("Remote player linked (deferred)!");
                    m_nativeHud.LogStep("OK", "Entity " + std::to_string(spawnReq.netId) + " linked (deferred)");
                } else {
                // ES: ── Rutas de spawn por factory (solo si falló el enlace al personaje del mod) ──
                // ── Factory spawn paths (only if mod link missed) ──

                // ES: Mapear el PlayerID del dueño a un slot de plantilla del mod (base 0).
                // Map owner PlayerID to mod template slot (0-based).
                int templateCount = m_spawnManager.GetModTemplateCount();
                int modSlot = 0;
                if (templateCount > 0 && spawnReq.owner > 0) {
                    modSlot = (static_cast<int>(spawnReq.owner) - 1) % templateCount;
                }
                if (modSlot < 0 || modSlot >= templateCount) modSlot = 0;

                void* newChar = nullptr;
                bool usedModTemplate = false;

                // ES: ── PATH 1: plantilla del mod (preferida — apariencia correcta) ──
                // ── PATH 1: Mod template (preferred — correct appearance) ──
                if (hasModTemplates) {
                    spdlog::info("Core: MOD TEMPLATE SPAWN for entity {} owner={} slot={} "
                                 "pos=({:.1f},{:.1f},{:.1f})",
                                 spawnReq.netId, spawnReq.owner, modSlot,
                                 spawnReq.position.x, spawnReq.position.y, spawnReq.position.z);

                    newChar = m_spawnManager.SpawnCharacterDirect(&spawnReq.position, modSlot);
                    if (newChar) usedModTemplate = true;
                }

                // ES: ── PATH 2: createRandomChar (respaldo inmediato — apariencia incorrecta) ──
                // ── PATH 2: createRandomChar (immediate fallback — wrong appearance) ──
                if (!newChar) {
                    spdlog::info("Core: createRandomChar FALLBACK for entity {} owner={} "
                                 "(modTemplate {})",
                                 spawnReq.netId, spawnReq.owner,
                                 hasModTemplates ? "failed" : "not available");

                    newChar = entity_hooks::CallFactoryCreateRandom(m_spawnManager.GetFactory());
                }

                // ES: Si el spawn devolvió un puntero plausible: arreglar facción (solo para random), enlazar, setup
                //     post-spawn, nombre de GameData (solo plantilla del mod), proxy visual, salud extendida y alianza.
                //     Si ambos caminos fallan, se reencola hasta MAX_SPAWN_RETRIES y luego se da de baja la entidad.
                // EN: If the spawn returned a plausible pointer: fix faction (random only), link, post-spawn setup,
                //     GameData name (mod template only), visual proxy, extended health and alliance. If both paths fail,
                //     it is requeued up to MAX_SPAWN_RETRIES and then the entity is unregistered.
                uintptr_t newCharAddr = reinterpret_cast<uintptr_t>(newChar);
                if (newChar && newCharAddr > 0x10000 && newCharAddr < 0x00007FFFFFFFFFFF
                    && (newCharAddr & 0x7) == 0) {
                    spdlog::info("Core: Spawn SUCCESS — char 0x{:X} for entity {} (modTemplate={})",
                                 newCharAddr, spawnReq.netId, usedModTemplate);

                    // ES: Solo arreglar la facción en spawns random: los de plantilla del mod tienen facción persistente de
                    //     kenshi-online.mod; escribirles la facción del jugador local los mete en el panel de escuadrón y peta.
                    // Only apply faction fix for non-mod-template spawns (random chars).
                    // Mod template characters have persistent factions from kenshi-online.mod
                    // that are always loaded. Writing the LOCAL player's faction causes them
                    // to appear in the squad panel, flooding it and crashing the game.
                    if (!usedModTemplate) {
                        SEH_FixUpFaction_Core(newChar);
                    }
                    m_entityRegistry.SetGameObject(spawnReq.netId, newChar);
                    m_entityRegistry.UpdatePosition(spawnReq.netId, spawnReq.position);
                    SEH_FallbackPostSpawnSetup(newChar, spawnReq.netId, spawnReq.owner, spawnReq.position);

                    // ES: Solo escribir el nombre de la plantilla GameData si se usó plantilla del mod (única por jugador);
                    //     createRandomChar puede compartir plantilla con otros NPC.
                    // Only write to GameData template if we used a mod template (unique per player).
                    // createRandomChar may share templates with other NPCs — skip to avoid corruption.
                    if (usedModTemplate) {
                        m_playerController.WriteGameDataNameForModLink(newChar, spawnReq.owner);
                    }

                    // ES: Crear el VisualProxy (seguimiento de estado + render futuro).
                    // Create VisualProxy for state tracking + future rendering
                    {
                        auto* rp = m_playerController.GetRemotePlayer(spawnReq.owner);
                        std::string displayName = rp ? rp->playerName
                            : ("Player_" + std::to_string(spawnReq.owner));
                        Quat spawnRot = spawnReq.rotation;
                        m_visualProxy.CreateProxy(spawnReq.netId, spawnReq.owner,
                            displayName, "", spawnReq.position, spawnRot);
                    }

                    // ES: Aplicar el estado de salud extendido si lo mandó el servidor.
                    // Apply extended health state if server provided it
                    if (spawnReq.hasExtendedState) {
                        m_entityRegistry.UpdateLimbHealth(spawnReq.netId, spawnReq.health);
                        if (!SEH_WriteLimbHealthDirect(newChar, spawnReq.health)) {
                            spdlog::warn("Core: Health write failed for spawned entity {}", spawnReq.netId);
                        }
                    }

                    // ES: Aliar la facción del mod con el jugador local (placa de nombre verde).
                    // Set mod faction allied with local player (green nameplate)
                    SEH_AllyModFaction(newChar);

                    s_directSpawnAttempts++;
                    s_directSpawnsPerPlayer[spawnReq.owner]++;
                    if (usedModTemplate) {
                        m_nativeHud.AddSystemMessage("Remote player spawned!");
                    } else {
                        m_nativeHud.AddSystemMessage("Remote player spawned (fallback appearance)");
                    }
                    m_nativeHud.LogStep("OK", "Entity " + std::to_string(spawnReq.netId) + " spawned");
                } else {
                    spdlog::warn("Core: Both spawn paths returned null for entity {} (attempt {})",
                                 spawnReq.netId, spawnReq.retryCount);
                    spawnReq.retryCount++;
                    if (spawnReq.retryCount < MAX_SPAWN_RETRIES) {
                        m_spawnManager.RequeueSpawn(spawnReq);
                    } else {
                        spdlog::error("Core: Entity {} exceeded max retries ({}), removing",
                                      spawnReq.netId, MAX_SPAWN_RETRIES);
                        // ES: ARREGLO BUG 2+3: decrementar el contador de spawns y limpiar la interpolación al dar de baja.
                        // BUG 2+3 FIX: Decrement spawn cap and clean up interpolation
                        entity_hooks::DecrementSpawnCount(spawnReq.owner);
                        m_interpolation.RemoveEntity(spawnReq.netId);
                        m_entityRegistry.Unregister(spawnReq.netId);
                        m_nativeHud.AddSystemMessage("Failed to spawn remote player after retries.");
                    }
                }

                } // end factory spawn else (PATH 0 missed)
                } // end spawn cap else
            }
        } else if (!hasFactory) {
            // ES: Factory aún no capturado — reintentar activando el hook CharacterCreate (a los 5 s) y avisar cada 5 s.
            // Factory not captured yet — try re-enabling CharacterCreate hook.
            if (!s_retriedHookEnable && pendingDuration.count() >= 5) {
                s_retriedHookEnable = true;
                spdlog::warn("Core: Factory not captured after 5s — re-enabling CharacterCreate hook");
                entity_hooks::ResumeForNetwork();
                m_nativeHud.LogStep("SPAWN", "Re-enabling CharacterCreate hook...");
            }
            if (pendingDuration.count() / 5 != s_lastNotReadyLog) {
                s_lastNotReadyLog = pendingDuration.count() / 5;
                spdlog::warn("Core: {} spawns pending for {}s but factory not ready "
                             "(factory={}, templates={}, modTemplates={})",
                             pending, pendingDuration.count(),
                             m_spawnManager.IsReady(),
                             m_spawnManager.GetTemplateCount(), m_spawnManager.GetModTemplateCount());
            }
        }
    }

    // ES: Quitar el flag de bypass forzado tras procesar.
    // Clear force bypass flag after processing
    if (m_forceSpawnBypass.load()) {
        m_forceSpawnBypass.store(false);
        spdlog::info("Core: Force spawn bypass cleared");
    }

    // ES: ═══ LIMPIEZA DE ENTIDADES ATASCADAS ═══ Cada 10 s: remotos registrados en estado Spawning sin objeto
    //     del juego y con la cola de spawns vacía se dan de baja (huérfanos).
    // ═══ STUCK ENTITY CLEANUP ═══
    // Detect remote entities registered but never spawned (stuck in Spawning state).
    // This catches entities that fell through the cracks — e.g., spawn queue drained
    // but SetGameObject was never called.
    static auto s_lastStuckCheck = std::chrono::steady_clock::now();
    auto sinceStuckCheck = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - s_lastStuckCheck);
    if (sinceStuckCheck.count() >= 10) {
        s_lastStuckCheck = std::chrono::steady_clock::now();
        auto remoteIds = m_entityRegistry.GetRemoteEntities();
        for (EntityID rid : remoteIds) {
            auto info = m_entityRegistry.GetInfo(rid);
            if (info && info->state == EntityState::Spawning && info->gameObject == nullptr) {
                // If spawn queue is empty, this entity is truly orphaned
                if (m_spawnManager.GetPendingSpawnCount() == 0) {
                    spdlog::warn("Core: Stuck entity {} (owner={}) in Spawning state with no "
                                 "game object and empty spawn queue — unregistering",
                                 rid, info->ownerPlayerId);
                    // BUG 2+3 FIX: Decrement spawn cap and clean up interpolation
                    entity_hooks::DecrementSpawnCount(info->ownerPlayerId);
                    m_interpolation.RemoveEntity(rid);
                    m_entityRegistry.Unregister(rid);
                }
            }
        }
    }
}

// ES: Rama legacy: encola en el orquestador el trabajo de fondo del frame (leer entidades locales e
//     interpolar remotas).
// EN: Legacy branch: posts the frame background work to the orchestrator (read local entities and
//     interpolate remote ones).
void Core::KickBackgroundWork() {
    m_orchestrator.PostFrameWork([this] { BackgroundReadEntities(); });
    m_orchestrator.PostFrameWork([this] { BackgroundInterpolate(); });
}

// ES: Registra cada 5 s cuántos ticks por segundo corre OnGameTick y el número de entidades.
// EN: Logs every 5 s how many ticks OnGameTick ran and the entity count.
void Core::UpdateDiagnostics(float deltaTime) {
    static int s_tickCount = 0;
    static auto s_lastTickLog = std::chrono::steady_clock::now();
    s_tickCount++;
    auto tickNow = std::chrono::steady_clock::now();
    auto tickElapsed = std::chrono::duration_cast<std::chrono::seconds>(tickNow - s_lastTickLog);
    if (tickElapsed.count() >= 5) {
        spdlog::info("Core::OnGameTick: {} ticks in last {}s (dt={:.4f}), entities={}, remote={}",
                      s_tickCount, tickElapsed.count(), deltaTime,
                      m_entityRegistry.GetEntityCount(),
                      m_entityRegistry.GetRemoteCount());
        s_tickCount = 0;
        s_lastTickLog = tickNow;
    }
}

// ES: ════ Métodos de los workers de fondo (corren en hilos del orquestador) ════
// ════════════════════════════════════════════════════════════════════════════
// Background Worker Methods (run on orchestrator threads)
// ════════════════════════════════════════════════════════════════════════════

// ES: Lectura SEH de todos los datos de un personaje necesarios para sincronizar posición. Corre en un
//     worker: el juego puede liberar personajes en cualquier momento; sin SEH una AV mataría el proceso.
//     Solo tipos POD dentro de __try.
// SEH-protected read of all character data needed for position sync.
// Runs on background worker thread — game can free characters at any time,
// turning valid pointers into dangling ones. Without SEH, an AV here kills
// the process. Only POD types in __try block (safe with MSVC SEH).
struct BGReadResult {
    Vec3 pos;
    Quat rot;
    float speed;
    uint8_t animState;
    bool valid;
};

static BGReadResult SEH_ReadCharacterBG(void* gameObj) {
    BGReadResult r = {};
    __try {
        game::CharacterAccessor character(gameObj);
        if (!character.IsValid()) return r;
        r.pos = character.GetPosition();
        r.rot = character.GetRotation();
        r.speed = character.GetMoveSpeed();
        r.animState = character.GetAnimState();
        r.valid = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_bgReadCrash = 0;
        if (++s_bgReadCrash <= 10) {
            char buf[128];
            sprintf_s(buf, "KMP: SEH caught AV in BGReadCharacter for gameObj 0x%p\n", gameObj);
            OutputDebugStringA(buf);
        }
        r.valid = false;
    }
    return r;
}

// ES: Worker (rama legacy): lee las entidades LOCALES, descarta las que no se movieron, calcula velocidad y
//     estado de animación, las guarda en el buffer de escritura y construye un paquete C2S_PositionUpdate
//     pre-serializado (lo enviaba SendCachedPackets, hoy sin uso).
// EN: Worker (legacy branch): reads the LOCAL entities, skips those that did not move, computes speed and
//     animation state, stores them in the write buffer and builds a pre-serialized C2S_PositionUpdate
//     packet (sent by SendCachedPackets, now unused).
void Core::BackgroundReadEntities() {
    auto& writeFrame = m_frameData[m_writeBuffer.load()];

    auto localEntities = m_entityRegistry.GetPlayerEntities(m_localPlayerId);

    static bool s_firstIterLog = true;
    if (s_firstIterLog && !localEntities.empty()) {
        spdlog::info("Core: Background read — {} local entities in registry", localEntities.size());
        s_firstIterLog = false;
    }

    struct PendingPos {
        CharacterPosition cp;
        EntityID netId;
        Vec3 pos;
        Quat rot;
    };
    std::vector<PendingPos> pendingPositions;

    for (EntityID netId : localEntities) {
        void* gameObj = m_entityRegistry.GetGameObject(netId);
        if (!gameObj) continue;

        // GetInfo() returns by value (std::optional<EntityInfo>) — thread-safe, no dangling pointer risk
        auto infoCopyOpt = m_entityRegistry.GetInfo(netId);
        if (!infoCopyOpt || infoCopyOpt->isRemote) continue;
        EntityInfo infoCopy = *infoCopyOpt;

        // ES: Lectura con SEH: el juego puede liberar personajes durante transiciones de zona.
        // SEH-protected read: game can free characters during zone transitions
        // while this background thread is reading. Dangling pointer → AV.
        BGReadResult rd = SEH_ReadCharacterBG(gameObj);
        if (!rd.valid) continue;

        Vec3 pos = rd.pos;
        Quat rot = rd.rot;

        if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) continue;

        float dist = pos.DistanceTo(infoCopy.lastPosition);
        if (dist < KMP_POS_CHANGE_THRESHOLD) continue;

        float computedSpeed = 0.f;
        if (infoCopy.lastUpdateTick > 0) {
            // ES: Velocidad nominal asumiendo ~60 fps.
            // Use a nominal deltaTime for background computation
            computedSpeed = dist / 0.016f; // ~60fps assumption
        }

        float speed = rd.speed;
        if (speed <= 0.f && computedSpeed > 0.f) {
            speed = computedSpeed;
        }

        uint8_t animState = rd.animState;
        if (animState == 0 && speed > 0.5f) {
            animState = (speed > 5.0f) ? 2 : 1;
        }

        // ES: Guardar en los datos del frame y preparar la entrada del paquete.
        // Store in frame data
        CachedEntityPos cached;
        cached.netId = netId;
        cached.position = pos;
        cached.rotation = rot;
        cached.speed = speed;
        cached.animState = animState;
        cached.dirty = true;
        writeFrame.localEntities.push_back(cached);

        PendingPos pp;
        pp.cp.entityId = netId;
        pp.cp.generation = infoCopy.generation; // Phase 6: send current generation
        pp.cp.posX = pos.x;
        pp.cp.posY = pos.y;
        pp.cp.posZ = pos.z;
        pp.cp.compressedQuat = rot.Compress();
        pp.cp.animStateId = animState;
        pp.cp.moveSpeed = static_cast<uint8_t>(std::min(255.f, speed / 15.f * 255.f));
        pp.cp.flags = (speed > 3.0f) ? 0x01 : 0x00;
        pp.netId = netId;
        pp.pos = pos;
        pp.rot = rot;
        pendingPositions.push_back(pp);
    }

    // ES: Construir el paquete pre-serializado.
    // Build pre-serialized packet
    if (!pendingPositions.empty()) {
        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_PositionUpdate);
        writer.WriteU8(static_cast<uint8_t>(pendingPositions.size()));
        for (auto& pp : pendingPositions) {
            writer.WriteRaw(&pp.cp, sizeof(pp.cp));
            // Update registry tracking (shared_mutex inside)
            m_entityRegistry.UpdatePosition(pp.netId, pp.pos);
            m_entityRegistry.UpdateRotation(pp.netId, pp.rot);
        }
        writeFrame.packetBytes = std::move(writer.Buffer());
    }

    writeFrame.ready = true;
}

// ES: Worker (rama legacy): calcula la interpolación de todas las entidades remotas y la guarda en el
//     buffer de escritura para que ApplyRemotePositions la aplique.
// EN: Worker (legacy branch): computes the interpolation of every remote entity and stores it in the write
//     buffer for ApplyRemotePositions to apply.
void Core::BackgroundInterpolate() {
    auto& writeFrame = m_frameData[m_writeBuffer.load()];

    auto remoteEntities = m_entityRegistry.GetRemoteEntities();
    float now = SessionTime();

    static int s_interpCallCount = 0;
    int validCount = 0;
    s_interpCallCount++;

    for (EntityID remoteId : remoteEntities) {
        CachedRemoteResult result;
        result.netId = remoteId;

        uint8_t moveSpeed = 0;
        uint8_t animState = 0;
        if (m_interpolation.GetInterpolated(remoteId, now,
                                             result.position, result.rotation,
                                             moveSpeed, animState)) {
            result.moveSpeed = moveSpeed;
            result.animState = animState;
            result.valid = true;
            validCount++;
        }

        writeFrame.remoteResults.push_back(result);
    }

    // Log first few calls + every 200th
    if (!remoteEntities.empty() && (s_interpCallCount <= 5 || s_interpCallCount % 200 == 0)) {
        spdlog::info("Core::BackgroundInterpolate: {} remote entities, {} valid interp results (call #{})",
                     remoteEntities.size(), validCount, s_interpCallCount);
    }
}

// ES: Comando /forcespawn: si hay spawns pendientes activa el bypass del gate para el próximo tick; si no,
//     reencola los remotos que no tienen objeto del juego.
// EN: /forcespawn command: if spawns are pending it enables the gate bypass for the next tick; otherwise it
//     requeues the remotes without a game object.
void Core::ForceSpawnRemotePlayers() {
    if (!m_connected) {
        m_nativeHud.AddSystemMessage("Not connected to a server.");
        return;
    }
    size_t pending = m_spawnManager.GetPendingSpawnCount();
    if (pending == 0) {
        m_nativeHud.AddSystemMessage("No pending spawns in queue.");
        // Check for remote entities without game objects
        auto remoteEntities = m_entityRegistry.GetRemoteEntities();
        int noObj = 0;
        for (auto eid : remoteEntities) {
            if (!m_entityRegistry.GetGameObject(eid)) noObj++;
        }
        if (noObj > 0) {
            m_nativeHud.AddSystemMessage(std::to_string(noObj) + " remote entities without game objects — re-queuing...");
            for (auto eid : remoteEntities) {
                if (!m_entityRegistry.GetGameObject(eid)) {
                    auto infoCopy = m_entityRegistry.GetInfo(eid);
                    if (infoCopy) {
                        SpawnRequest req;
                        req.netId = eid;
                        req.owner = infoCopy->ownerPlayerId;
                        req.type = infoCopy->type;
                        req.position = infoCopy->lastPosition;
                        m_spawnManager.QueueSpawn(req);
                    }
                }
            }
        }
        return;
    }
    spdlog::info("Core::ForceSpawnRemotePlayers: {} pending spawns, forcing bypass", pending);
    m_nativeHud.AddSystemMessage("Force-spawning " + std::to_string(pending) + " remote player(s)...");
    m_forceSpawnBypass.store(true);
    // HandleSpawnQueue will run next tick with the bypass active
}

// ES: Comando /tp: busca la entidad remota más cercana (con posición válida) al primer personaje local y
//     teletransporta allí todo el escuadrón local (rejilla de 3 m). Devuelve true si movió alguno.
// EN: /tp command: finds the remote entity closest (with a valid position) to the first local character and
//     teleports the whole local squad there (3 m grid). Returns true if any was moved.
bool Core::TeleportToNearestRemotePlayer() {
    if (!m_connected) {
        m_nativeHud.AddSystemMessage("Not connected to a server.");
        return false;
    }

    // Find the nearest remote entity with a valid game object and position
    auto remoteEntities = m_entityRegistry.GetRemoteEntities();
    if (remoteEntities.empty()) {
        m_nativeHud.AddSystemMessage("No remote players found.");
        spdlog::info("Core: TeleportToNearest — no remote entities");
        return false;
    }

    // Get our first local entity's position as reference
    auto localEntities = m_entityRegistry.GetPlayerEntities(m_localPlayerId);
    if (localEntities.empty()) {
        m_nativeHud.AddSystemMessage("No local characters registered.");
        return false;
    }

    Vec3 localPos(0, 0, 0);
    void* firstLocalObj = m_entityRegistry.GetGameObject(localEntities[0]);
    if (firstLocalObj) {
        game::CharacterAccessor localAccessor(firstLocalObj);
        localPos = localAccessor.GetPosition();
    }

    // Find the nearest remote entity with a valid position
    float bestDist = 1e18f;
    Vec3 bestPos(0, 0, 0);
    EntityID bestId = INVALID_ENTITY;
    PlayerID bestOwner = 0;

    for (EntityID remoteId : remoteEntities) {
        auto infoCopy = m_entityRegistry.GetInfo(remoteId);
        if (!infoCopy) continue;

        Vec3 rPos = infoCopy->lastPosition;
        if (rPos.x == 0.f && rPos.y == 0.f && rPos.z == 0.f) continue;

        float dist = localPos.DistanceTo(rPos);
        if (dist < bestDist) {
            bestDist = dist;
            bestPos = rPos;
            bestId = remoteId;
            bestOwner = infoCopy->ownerPlayerId;
        }
    }

    if (bestId == INVALID_ENTITY) {
        m_nativeHud.AddSystemMessage("No remote players with valid positions found.");
        return false;
    }

    // Get the remote player's name for display
    auto* rp = m_playerController.GetRemotePlayer(bestOwner);
    std::string targetName = rp ? rp->playerName : ("Player_" + std::to_string(bestOwner));

    // Teleport all local entities to the target position
    int teleported = 0;
    for (EntityID netId : localEntities) {
        void* gameObj = m_entityRegistry.GetGameObject(netId);
        if (!gameObj) continue;

        game::CharacterAccessor accessor(gameObj);
        if (!accessor.IsValid()) continue;

        Vec3 tpPos = bestPos;
        tpPos.x += static_cast<float>(teleported % 4) * 3.0f;
        tpPos.z += static_cast<float>(teleported / 4) * 3.0f;

        if (accessor.WritePosition(tpPos)) {
            m_entityRegistry.UpdatePosition(netId, tpPos);
            teleported++;
        }
    }

    if (teleported > 0) {
        spdlog::info("Core: Teleported {} characters to {} at ({:.1f}, {:.1f}, {:.1f}) [dist={:.0f}]",
                     teleported, targetName, bestPos.x, bestPos.y, bestPos.z, bestDist);
        m_nativeHud.AddSystemMessage("Teleported to " + targetName + "!");
        m_overlay.AddSystemMessage("Teleported " + std::to_string(teleported) +
                                   " characters to " + targetName);
        return true;
    }

    m_nativeHud.AddSystemMessage("Teleport failed — no valid local characters.");
    return false;
}

} // namespace kmp
