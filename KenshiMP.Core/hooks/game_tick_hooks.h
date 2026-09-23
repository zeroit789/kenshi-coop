// ES: Interfaz del hook del tick de frame del juego ("GameFrameUpdate", RVA 0x123A10 en
//     Steam 1.0.68). Hoy es principalmente diagnóstico: el tick real del mod lo mueve el
//     hook de Present de render_hooks.cpp.
// EN: Interface of the game frame tick hook ("GameFrameUpdate", RVA 0x123A10 on Steam
//     1.0.68). It is mainly diagnostic today: the mod's real tick is driven by the
//     Present hook in render_hooks.cpp.
#pragma once

// ES: Espacio de nombres del hook de tick de frame.
// EN: Namespace for the frame tick hook.
namespace kmp::game_tick_hooks {

// ES: Instala el hook sobre GameFrameUpdate; false si la función no se resolvió o falla MinHook.
// EN: Installs the hook on GameFrameUpdate; false if the function was not resolved or MinHook fails.
bool Install();
// ES: Quita el hook GameFrameUpdate.
// EN: Removes the GameFrameUpdate hook.
void Uninstall();

} // namespace kmp::game_tick_hooks
