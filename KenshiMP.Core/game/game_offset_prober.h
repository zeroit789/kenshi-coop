// ES: game_offset_prober.h - API del sondeador de offsets en tiempo de ejecución: descubre
//     offsets desconocidos de CharacterOffsets inspeccionando personajes vivos del juego y los
//     guarda en una caché JSON junto al ejecutable para no repetir el sondeo.
// EN: game_offset_prober.h - Runtime offset prober API: discovers unknown CharacterOffsets
//     by inspecting live game characters and stores them in a JSON cache next to the executable
//     so probing is not repeated.
#pragma once
#include <cstdint>

namespace kmp::game {

// ═══════════════════════════════════════════════════════════════════════════
//  RUNTIME OFFSET PROBER
// ═══════════════════════════════════════════════════════════════════════════
// ES: Descubre offsets desconocidos de CharacterOffsets sondeando objetos vivos; se ejecuta tras
//     tener el primer personaje válido (después de cargar). Los resultados se cachean y se
//     recargan en el siguiente arranque si el ejecutable no ha cambiado.
//     OJO: el fichero real se llama "KenshiOnline_offset_cache.json" (no offset_cache.json).
// EN: NOTE: the actual file name is "KenshiOnline_offset_cache.json" (not offset_cache.json).
// Discovers unknown CharacterOffsets at runtime by probing live game objects.
// Runs once after the first valid character is available (post-game-load).
// Results are cached to offset_cache.json and reloaded on next launch if
// the game executable hasn't changed.

// ES: Ejecuta todas las sondas sobre un personaje (se puede llamar varias veces; lleva la cuenta
//     de lo ya sondeado). True si se descubrió al menos un offset NUEVO en esta llamada.
//     charPtr debe ser un Character* válido con posición no nula; npcCharPtr es un NPC opcional
//     para la sonda diferencial de isPlayerControlled (hoy neutralizada), 0 si no hay.
// EN:
// Run the full probe suite on a character pointer. Safe to call repeatedly;
// internally tracks whether probing has already completed.
// Returns true if at least one NEW offset was discovered this call.
// |charPtr| must be a valid KCharacter* with a non-zero position.
// |npcCharPtr| optional NPC character for differential probing (isPlayerControlled).
//              Pass 0 if no NPC is available yet.
bool RunOffsetProber(uintptr_t charPtr, uintptr_t npcCharPtr = 0);

// ES: Intenta cargar los offsets cacheados; true si la caché era válida y se restauraron.
//     Llamar pronto en el arranque (antes de RunOffsetProber).
// EN:
// Try to load previously cached offsets from offset_cache.json.
// Returns true if cache was valid and offsets were restored.
// Call early in startup (before RunOffsetProber).
bool LoadOffsetCache();

// ES: Guarda en la caché los offsets descubiertos (RunOffsetProber lo llama solo).
// EN:
// Save currently discovered offsets to offset_cache.json.
// Called automatically by RunOffsetProber after successful discovery.
void SaveOffsetCache();

// ES: Reinicia el estado del sondeador (al desconectar/reconectar o en una segunda carga).
// EN:
// Reset all prober state (call on disconnect/reconnect/second game load).
void ResetOffsetProber();

// ES: True si el sondeador terminó (todas las sondas posibles intentadas).
// EN:
// Returns true if the prober has completed (all feasible offsets discovered or attempted).
bool IsProberComplete();

} // namespace kmp::game
