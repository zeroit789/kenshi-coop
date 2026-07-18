#pragma once
#include <cstdint>

namespace kmp::ai_hooks {

bool Install();
void Uninstall();

// Remote-controlled character tracking.
// Characters marked as remote get their AI decisions overridden (not suppressed).
void MarkRemoteControlled(void* character);
void UnmarkRemoteControlled(void* character);
bool IsRemoteControlled(void* character);

// Vacía todo el set de remote-controlled (llamado en desconexión / recarga de save en caliente).
void ClearRemoteControlled();

// Diagnóstico (solo lectura): nº de chars actualmente marcados como remote-controlled.
// Usado por [DIAG-REMOTE] en core.cpp para detectar si el char del host fue marcado por error.
size_t RemoteControlledCount();

} // namespace kmp::ai_hooks
