// ES: Interfaz de los hooks de movimiento: CharacterSetPosition (HavokCharacter::setPosition,
//     RVA 0x145E50) y CharacterMoveTo (orden de movimiento con pathfinding, RVA 0x2EF4E3).
//     Ninguno se instala hoy (prólogo `mov rax,rsp` + llamadas masivas / 5º parámetro en
//     pila); la sincronización de posición se hace por sondeo desde Core::OnGameTick.
// EN: Interface of the movement hooks: CharacterSetPosition (HavokCharacter::setPosition,
//     RVA 0x145E50) and CharacterMoveTo (pathfinding move order, RVA 0x2EF4E3). Neither is
//     installed today (`mov rax,rsp` prologue + massive call rate / 5th stack parameter);
//     position sync is done by polling from Core::OnGameTick.
#pragma once

// ES: Espacio de nombres de los hooks de movimiento.
// EN: Namespace for the movement hooks.
namespace kmp::movement_hooks {

// ES: Solo registra en el log las direcciones encontradas; no engancha nada. Devuelve true.
// EN: Only logs the addresses found; hooks nothing. Returns true.
bool Install();
// ES: Quita los hooks por nombre (inofensivo si no se instalaron).
// EN: Removes the hooks by name (harmless if not installed).
void Uninstall();

} // namespace kmp::movement_hooks
