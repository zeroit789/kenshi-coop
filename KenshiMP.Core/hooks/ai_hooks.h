// ES: Interfaz de los hooks de IA: AICreate (creación del controlador de IA de un
//     personaje, RVA 0x622110) y AIPackages (carga de paquetes de comportamiento, RVA
//     0x271620). Además mantiene el conjunto compartido de personajes "controlados en
//     remoto" (de otros jugadores) cuya IA no debe decidir por ellos.
// EN: Interface of the AI hooks: AICreate (creation of a character's AI controller,
//     RVA 0x622110) and AIPackages (behavior package loading, RVA 0x271620). It also keeps
//     the shared set of "remote-controlled" characters (owned by other players) whose AI
//     must not make decisions for them.
#pragma once
#include <cstdint>

// ES: Espacio de nombres de los hooks de IA.
// EN: Namespace for the AI hooks.
namespace kmp::ai_hooks {

// ES: Instala AICreate y AIPackages si se resolvieron; true si al menos uno quedó instalado.
// EN: Installs AICreate and AIPackages if resolved; true if at least one was installed.
bool Install();
// ES: Quita ambos hooks y vacía el conjunto de remotos.
// EN: Removes both hooks and clears the remote set.
void Uninstall();

// ES: Seguimiento de personajes controlados en remoto. A los marcados se les sobrescriben
//     (no se suprimen) las decisiones de IA. Todas estas funciones son seguras entre hilos (mutex).
// Remote-controlled character tracking.
// Characters marked as remote get their AI decisions overridden (not suppressed).
void MarkRemoteControlled(void* character);
void UnmarkRemoteControlled(void* character);
bool IsRemoteControlled(void* character);

// EN: Empties the whole remote-controlled set (called on disconnect / hot save reload).
// Vacía todo el set de remote-controlled (llamado en desconexión / recarga de save en caliente).
void ClearRemoteControlled();

// EN: Diagnostics (read-only): number of chars currently marked as remote-controlled.
//     Used by [DIAG-REMOTE] in core.cpp to detect whether the host's char was wrongly marked.
// Diagnóstico (solo lectura): nº de chars actualmente marcados como remote-controlled.
// Usado por [DIAG-REMOTE] en core.cpp para detectar si el char del host fue marcado por error.
size_t RemoteControlledCount();

} // namespace kmp::ai_hooks
