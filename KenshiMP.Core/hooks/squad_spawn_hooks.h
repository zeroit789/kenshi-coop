// ES: Interfaz del hook "SquadSpawnBypass" (comprobación de spawn de escuadras del juego,
//     RVA 0x4FF47C en Steam 1.0.68). Cuando el mod tiene spawns de red pendientes, el hook
//     fuerza al juego a crear el NPC de la escuadra aunque normalmente lo saltaría; luego
//     entity_hooks "secuestra" ese NPC para representar al personaje remoto.
// EN: Interface of the "SquadSpawnBypass" hook (the game's squad spawn check, RVA 0x4FF47C
//     on Steam 1.0.68). When the mod has pending network spawns, the hook forces the game to
//     create the squad NPC even if it would normally skip it; entity_hooks then "hijacks"
//     that NPC to represent the remote character.
#pragma once
#include "kmp/types.h"
#include <queue>
#include <mutex>
#include <atomic>

// ES: Espacio de nombres del bypass de spawn de escuadras.
// EN: Namespace for the squad spawn bypass.
namespace kmp::squad_spawn_hooks {

// ES: Instala el hook del bypass de spawn; false si la dirección no se resolvió o falla.
// Install hooks for squad spawn bypass
bool Install();
// ES: Quita el hook.
// EN: Removes the hook.
void Uninstall();

// ES: Encola una petición de spawn (cola local heredada). La próxima vez que el juego
//     evalúe el spawn de una escuadra, el hook forzará el spawn por la vía natural.
// Queue a character spawn request. When the game next evaluates squad spawning,
// the hook will force-spawn this character through the natural pipeline.
void QueueSquadSpawn(void* gameData, const Vec3& position);

// ES: Número de spawns pendientes en la cola local.
// Get number of pending squad spawns
int GetPendingCount();

// ES: Total de bypass de spawn completados.
// Get total successful squad bypass spawns
int GetSuccessCount();

} // namespace kmp::squad_spawn_hooks
