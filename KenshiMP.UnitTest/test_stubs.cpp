// ES: Stubs solo para KenshiMP.UnitTest.
//
//     El test compila game_character.cpp directamente, y ese fichero llama a dos funciones
//     que viven en unidades del Core que NO se meten en el test (spawn_manager.cpp y
//     hooks/ai_hooks.cpp): arrastrarían MinHook, los hooks y el singleton del Core.
//     Aquí se dan implementaciones mínimas y deterministas para que el test enlace.
// EN: Stubs for KenshiMP.UnitTest only.
//
//     The test compiles game_character.cpp directly, and that file calls two functions
//     living in Core units that are NOT built into the test (spawn_manager.cpp and
//     hooks/ai_hooks.cpp): they would drag in MinHook, the hooks and the Core singleton.
//     Minimal, deterministic implementations are provided here so the test links.

#include "game/spawn_manager.h"
#include "hooks/ai_hooks.h"

#include <string>

namespace kmp {

// ES: En el test no hay memoria real de Kenshi que leer: devolver cadena vacía es lo mismo
//     que hace la versión real cuando la lectura protegida por SEH falla.
// EN: The test has no real Kenshi memory to read: returning an empty string matches what
//     the real version does when the SEH-protected read fails.
std::string SpawnManager::ReadKenshiString(uintptr_t /*addr*/) {
    return "";
}

namespace ai_hooks {

// ES: En el test no hay peers conectados, así que ningún personaje está controlado en remoto.
// EN: The test has no connected peers, so no character is remotely controlled.
bool IsRemoteControlled(void* /*character*/) {
    return false;
}

} // namespace ai_hooks
} // namespace kmp
