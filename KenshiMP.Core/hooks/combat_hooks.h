// ES: Hooks de combate. Interceptan la muerte (CharacterDeath, RVA 0x7A6200) y el KO
//     (CharacterKO, RVA 0x345C10) de personajes para avisar al servidor, más varios hooks de
//     SOLO diagnóstico sobre la ruta de órdenes y el AI tick de combate (Tasker::pushOrder,
//     validador Character::addOrder, CombatClass::update). Las RVAs son direcciones relativas a
//     la base de kenshi_x64.exe; el escáner las resuelve por patrón AOB/string ancla.
// EN: Combat hooks. They intercept character death (CharacterDeath, RVA 0x7A6200) and knockout
//     (CharacterKO, RVA 0x345C10) to notify the server, plus several DIAGNOSTIC-ONLY hooks on the
//     order path and the combat AI tick (Tasker::pushOrder, Character::addOrder validator,
//     CombatClass::update). RVAs are addresses relative to the kenshi_x64.exe base; the scanner
//     resolves them via AOB pattern/anchor string.
#pragma once
#include <cstdint>
#include <atomic>

// ES: Espacio de nombres de los hooks de combate.
// EN: Namespace for the combat hooks.
namespace kmp::combat_hooks {

// ES: Instala los hooks de combate (algunos arrancan deshabilitados). Siempre devuelve true.
// EN: Installs the combat hooks (some start disabled). Always returns true.
bool Install();
// ES: Quita todos los hooks de combate registrados en HookManager.
// EN: Removes all combat hooks registered in HookManager.
void Uninstall();

// EN: [DIAG-PUSHORDER] Host Tasker (char+0x658 ActivePlatoon → +0x98), published by the core's
//     [AUTOTEST] BEFORE firing attackTarget. The pushOrder hook compares it with the `this` of
//     each insertion to set isHost=1 when the order lands in the host's Tasker.
//     0 = not resolved yet. Written by the logic thread, read by the hook (same game thread).
// ES:
// [DIAG-PUSHORDER] Tasker del host (char+0x658 ActivePlatoon → +0x98) publicado por el
// [AUTOTEST] del core ANTES de disparar attackTarget. El hook de pushOrder lo compara con el
// `this` de cada inserción para marcar isHost=1 cuando la orden entra en el Tasker del host.
// 0 = aún no resuelto. Escrito por el hilo de lógica, leído por el hook (mismo hilo de juego).
extern std::atomic<uintptr_t> g_hostTaskerForDiag;

// EN: [DIAG-COMBATSEED] Host CombatClass (*(CharBody+0x8), with CharBody=*(hostChar+0x648)) and
//     its AI (*(hostChar+0x650)), published every tick by the LOGIC THREAD (ProcessDeferredEvents,
//     see PublishHostCombatDiag). The CombatClass::update hook (0x60D650) compares its `this` with
//     g_hostCombatClassForDiag to filter ONLY the host CombatClass ticks, and uses g_hostAiForDiag
//     to read AI+0x28 (inline AttackState). 0 = not resolved yet (menu/loading).
// ES:
// [DIAG-COMBATSEED] CombatClass del host (*(CharBody+0x8), con CharBody=*(hostChar+0x648)) y su
// AI (*(hostChar+0x650)), publicados cada tick por el HILO DE LÓGICA (ProcessDeferredEvents, ver
// PublishHostCombatDiag). El hook de CombatClass::update (0x60D650) compara su `this` con
// g_hostCombatClassForDiag para filtrar SOLO los ticks del CombatClass del host, y usa
// g_hostAiForDiag para leer AI+0x28 (AttackState inline). 0 = aún no resuelto (menú/carga).
extern std::atomic<uintptr_t> g_hostCombatClassForDiag;
extern std::atomic<uintptr_t> g_hostAiForDiag;

// ES: Procesa los eventos de combate diferidos (log, paquetes C2S, lectura de salud) desde el
//     contexto seguro del game tick. Se llama desde Core::OnGameTick, NUNCA dentro de un hook.
// EN: (original below)
// Process deferred combat events from the safe game-tick context.
// Called from Core::OnGameTick — NOT from inside a hook.
void ProcessDeferredEvents();

// ES: Supresión de eco: el packet handler activa estas banderas antes de llamar a
//     CharacterDeath/CharacterKO nativos por un evento que viene del servidor. Con la bandera
//     activa el hook no encola el evento, evitando el bucle infinito C2S→S2C→C2S.
// EN: (original below)
// Echo suppression: set before calling native CharacterDeath/CharacterKO from
// packet handler (server-sourced events). The hook checks this flag and skips
// pushing to the deferred queue, preventing infinite C2S→S2C→C2S echo loops.
void SetServerSourcedDeath(bool active);
void SetServerSourcedKO(bool active);

} // namespace kmp::combat_hooks
