// ES: Interfaz del módulo de escuadras. Los hooks previstos (SquadCreate, RVA 0x480B50, y
//     SquadAddMember, RVA 0x928423) NO se instalan por riesgo de crash en la carga de zonas;
//     el módulo conserva el puntero crudo a SquadAddMember para "inyectar" personajes remotos
//     en la escuadra del jugador local llamando directamente a la función del motor.
// EN: Interface of the squad module. The planned hooks (SquadCreate, RVA 0x480B50, and
//     SquadAddMember, RVA 0x928423) are NOT installed due to zone-load crash risk; the module
//     keeps the raw SquadAddMember pointer to "inject" remote characters into the local
//     player's squad by calling the engine function directly.
#pragma once
#include <cstdint>

// ES: Espacio de nombres de las escuadras.
// EN: Namespace for squads.
namespace kmp::squad_hooks {

// ES: No engancha nada; valida con .pdata la dirección de SquadAddMember y devuelve true si
//     el puntero crudo se puede usar.
// EN: Hooks nothing; validates the SquadAddMember address via .pdata and returns true if the
//     raw pointer is usable.
bool Install();
// ES: Quita hooks (si los hubiera) y vacía los mapas de escuadras.
// EN: Removes hooks (if any) and clears the squad maps.
void Uninstall();

// ES: Suprime los envíos de red durante la carga.
// Suppress network sends during loading
void SetLoading(bool loading);

// ES: La llama el packet handler cuando el servidor asigna un netId a una escuadra nueva:
//     asocia el puntero de escuadra más antiguo pendiente con ese id. Como SquadCreate no se
//     hookea, en la práctica no hay punteros pendientes y solo avisa en el log.
// EN: Note: since SquadCreate is not hooked, in practice there are no pending pointers and it
//     only logs a warning.
// Called by packet handler when the server assigns a net ID to a newly created squad.
// Maps the most recently created squad pointer to the server-assigned ID.
void OnSquadNetIdAssigned(uint32_t squadNetId);

// ES: Inyección en escuadra: añade un personaje a la escuadra del jugador local con la propia
//     función SquadAddMember del motor, para que sea seleccionable, reciba órdenes y salga en
//     el panel de escuadra. Devuelve true si funcionó; false si no hay escuadra o función.
// ── Squad Injection ──
// Adds a character to the local player's squad using the engine's own SquadAddMember function.
// This makes the character selectable, orderable, and visible in the squad panel.
// Returns true if injection succeeded, false if squad not found or function unavailable.
bool AddCharacterToLocalSquad(void* character);

} // namespace kmp::squad_hooks
