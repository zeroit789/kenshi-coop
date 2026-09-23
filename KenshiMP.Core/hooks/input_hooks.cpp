// ES: Implementación del módulo de hooks de entrada. No hookea ninguna función de
//     kenshi_x64.exe: las teclas del mod (Tab, Enter, F1, Escape, ~) se procesan en el
//     WndProc de render_hooks.cpp, que corre en el hilo de mensajes de la ventana.
//     Aquí solo queda la documentación de los atajos y un Install()/Uninstall() vacíos.
// EN: Implementation of the input hooks module. It does not hook any kenshi_x64.exe
//     function: mod keys (Tab, Enter, F1, Escape, ~) are processed in the WndProc from
//     render_hooks.cpp, which runs on the window message thread. Only the keybind
//     documentation and an empty Install()/Uninstall() remain here.
#include "input_hooks.h"
#include "../core.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <Windows.h>

namespace kmp::input_hooks {

// ES: La entrada se gestiona con el hook de WndProc de render_hooks.cpp.
//     Este módulo quedaría para procesado adicional de atajos (hoy no hace nada más).
// EN: Input hooks are handled through the WndProc hook in render_hooks.cpp.
//     This module handles additional keybind processing.

// ES: Atajos de teclado (implementados realmente en el WndProc de render_hooks.cpp;
//     allí además Insert abre el panel de log y F1 el menú nativo):
// EN: Keybinds (actually implemented in the render_hooks.cpp WndProc; there, Insert
//     also opens the log panel and F1 the native menu):
// Keybinds:
// Tab        - Toggle player list
// Enter      - Toggle chat
// F1         - Toggle connection UI
// Escape     - Close any open overlay panel
// Tilde (~)  - Toggle debug overlay

// ES: Indica si Install() se ha llamado (solo informativo).
// EN: Whether Install() has been called (informational only).
static bool s_installed = false;

// ES: No instala ningún hook real; solo marca el módulo como activo y lo registra en el log.
// EN: Installs no real hook; it only flags the module as active and logs it.
bool Install() {
    // ES: La entrada va principalmente por el hook de WndProc de render_hooks.
    //     Aquí se podrían añadir hooks de OIS si hiciera falta.
    // EN: Input is primarily handled via the WndProc hook in render_hooks.
    //     Additional OIS hooks can be added here if needed.
    s_installed = true;
    spdlog::info("input_hooks: Installed (using WndProc-based input)");
    return true;
}

// ES: Marca el módulo como desinstalado (no hay nada que desenganchar).
// EN: Flags the module as uninstalled (there is nothing to unhook).
void Uninstall() {
    s_installed = false;
}

} // namespace kmp::input_hooks
