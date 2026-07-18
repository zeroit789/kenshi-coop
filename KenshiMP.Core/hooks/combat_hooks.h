#pragma once
#include <cstdint>
#include <atomic>

namespace kmp::combat_hooks {

bool Install();
void Uninstall();

// [DIAG-PUSHORDER] Tasker del host (char+0x658 ActivePlatoon → +0x98) publicado por el
// [AUTOTEST] del core ANTES de disparar attackTarget. El hook de pushOrder lo compara con el
// `this` de cada inserción para marcar isHost=1 cuando la orden entra en el Tasker del host.
// 0 = aún no resuelto. Escrito por el hilo de lógica, leído por el hook (mismo hilo de juego).
extern std::atomic<uintptr_t> g_hostTaskerForDiag;

// [DIAG-COMBATSEED] CombatClass del host (*(CharBody+0x8), con CharBody=*(hostChar+0x648)) y su
// AI (*(hostChar+0x650)), publicados cada tick por el HILO DE LÓGICA (ProcessDeferredEvents, ver
// PublishHostCombatDiag). El hook de CombatClass::update (0x60D650) compara su `this` con
// g_hostCombatClassForDiag para filtrar SOLO los ticks del CombatClass del host, y usa
// g_hostAiForDiag para leer AI+0x28 (AttackState inline). 0 = aún no resuelto (menú/carga).
extern std::atomic<uintptr_t> g_hostCombatClassForDiag;
extern std::atomic<uintptr_t> g_hostAiForDiag;

// Process deferred combat events from the safe game-tick context.
// Called from Core::OnGameTick — NOT from inside a hook.
void ProcessDeferredEvents();

// Echo suppression: set before calling native CharacterDeath/CharacterKO from
// packet handler (server-sourced events). The hook checks this flag and skips
// pushing to the deferred queue, preventing infinite C2S→S2C→C2S echo loops.
void SetServerSourcedDeath(bool active);
void SetServerSourcedKO(bool active);

} // namespace kmp::combat_hooks
