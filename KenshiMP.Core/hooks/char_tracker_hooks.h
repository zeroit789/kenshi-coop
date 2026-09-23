// ES: Interfaz del rastreador de personajes por animación. Instala un hook INLINE manual
//     (no MinHook) en "CharAnimUpdate" (tick de actualización de animación de personaje,
//     RVA 0x65F6C7 en Steam 1.0.68) para descubrir todos los personajes vivos a partir de su
//     AnimationClassHuman y mantener un mapa puntero -> nombre/posición consultable por nombre.
// EN: Interface of the animation-based character tracker. Installs a manual INLINE hook
//     (not MinHook) at "CharAnimUpdate" (character animation update tick, RVA 0x65F6C7 on
//     Steam 1.0.68) to discover every live character from its AnimationClassHuman and keep a
//     pointer -> name/position map that can be queried by name.
#pragma once
#include "kmp/types.h"
#include <string>
#include <functional>

// ES: Espacio de nombres del rastreador de personajes.
// EN: Namespace for the character tracker.
namespace kmp::char_tracker_hooks {

// ES: Parchea el sitio de CharAnimUpdate con un salto a un trampolín propio; false si no se resolvió o falla.
// EN: Patches the CharAnimUpdate site with a jump to our own trampoline; false if unresolved or on failure.
bool Install();
// ES: Restaura los 14 bytes originales y libera el trampolín.
// EN: Restores the original 14 bytes and frees the trampoline.
void Uninstall();

// ES: Personaje descubierto: punteros a su AnimationClassHuman y a su CharacterHuman
//     (leído en animClass+0x2D8), nombre, posición al descubrirlo y último instante visto
//     (GetTickCount64, ms).
// EN: Discovered character: pointers to its AnimationClassHuman and CharacterHuman (read at
//     animClass+0x2D8), name, position at discovery time and last-seen time
//     (GetTickCount64, ms).
struct TrackedChar {
    void* animClassPtr;     // AnimationClassHuman*
    void* characterPtr;     // CharacterHuman* (at animClass+0x2D8)
    std::string name;
    Vec3 position;
    uint64_t lastSeenTick;
};

// ES: Búsqueda por nombre o por puntero de personaje; nullptr si no está. OJO: el puntero
//     devuelto apunta dentro del mapa y deja de estar protegido por el mutex al volver.
// EN: Lookup by name or by character pointer; nullptr if absent. NOTE: the returned pointer
//     points into the map and is no longer protected by the mutex once the call returns.
const TrackedChar* FindByName(const std::string& name);
const TrackedChar* FindByPtr(void* characterPtr);

// EN: Purges one tracker entry (called from the engine destroy hook).
// Purga una entrada concreta del tracker (llamado desde el destroy-hook del motor).
void RemoveByPtr(void* ptr);
// EN: Empties the whole tracker (called on disconnect / hot save reload).
// Vacía todo el tracker (llamado en desconexión / recarga de save en caliente).
void Clear();
// ES: AnimClass del jugador local (hoy nunca se asigna: siempre nullptr) y de un jugador
//     remoto por nombre; callback para personajes nuevos; recuento y volcado al log.
// EN: Local player's AnimClass (never assigned today: always nullptr) and a remote player's
//     by name; callback for new characters; count and log dump.
void* GetLocalPlayerAnimClass();
void* GetRemotePlayerAnimClass(const std::string& name);
void SetOnNewCharacter(std::function<void(const TrackedChar&)> callback);
int GetTrackedCount();
void DumpTrackedChars();

// ES: Procesa los descubrimientos diferidos desde el contexto seguro del tick
//     (Core::OnGameTick), NO desde dentro del hook inline.
// Process deferred character discoveries from safe game-tick context.
// Called from Core::OnGameTick — NOT from inside the inline hook.
void ProcessDeferredDiscovery();

} // namespace kmp::char_tracker_hooks
