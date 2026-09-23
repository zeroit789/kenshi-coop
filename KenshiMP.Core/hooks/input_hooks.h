// ES: Interfaz del módulo de hooks de entrada (teclado/ratón). En la práctica no
//     intercepta ninguna función de Kenshi: toda la entrada del mod se gestiona en el
//     WndProc que instala render_hooks.cpp. Este módulo existe como punto de extensión
//     (p. ej. futuros hooks de OIS).
// EN: Interface of the input hooks module (keyboard/mouse). In practice it does not
//     intercept any Kenshi function: all mod input is handled in the WndProc installed
//     by render_hooks.cpp. This module exists as an extension point (e.g. future OIS hooks).
#pragma once

// ES: Espacio de nombres de los hooks de entrada.
// EN: Namespace for the input hooks.
namespace kmp::input_hooks {

// ES: Marca el módulo como instalado (no engancha nada del juego). Siempre devuelve true.
// EN: Marks the module as installed (does not hook anything in the game). Always returns true.
bool Install();
// ES: Marca el módulo como desinstalado.
// EN: Marks the module as uninstalled.
void Uninstall();

} // namespace kmp::input_hooks
